#include "mod.h"

/*
 * Buffer cache: 单链表LRU设计 (仿照xv6 bio.c)
 *
 * head.next = MRU (最近释放的buffer)
 * head.prev = LRU (最久未释放的buffer)
 * buffer在被访问时不动位置, 只在释放(ref归零)时移到MRU
 *
 * 页面管理: data页通过pmem_alloc惰性分配, 总数限制在N_BUF_PAGES以内
 * 超出上限时从LRU端ref==0的buffer窃取页面
 */

static buffer_node_t buf_cache[N_BUFFER];
static buffer_node_t buf_head;
static spinlock_t lk_buf_cache;

#define N_BUF_PAGES 768 // buffer缓存最多持有的数据页数 (< KERN_PAGES=1024)
static int buf_npages = 0;

/* 前向声明 */
static void buffer_read(buffer_t *buf);

/* 磁盘读取: block -> buf */
static void buffer_read(buffer_t *buf)
{
    if (!sleeplock_holding(&buf->slk))
        panic("buffer_read: not holding sleeplock");
    virtio_disk_rw(buf, false);
}

/* 磁盘写入: buf -> block */
void buffer_write(buffer_t *buf)
{
    if (!sleeplock_holding(&buf->slk))
        panic("buffer_write: not holding sleeplock");
    virtio_disk_rw(buf, true);
}

/*
    buffer系统初始化:
    创建单条LRU双向循环链表
    所有buffer初始化后插入链表, data指针均为NULL
*/
void buffer_init()
{
    spinlock_init(&lk_buf_cache, "lk_buf_cache");

    // 哨兵节点
    buf_head.next = &buf_head;
    buf_head.prev = &buf_head;

    // 所有buffer插入MRU端 (head->next)
    for (int i = 0; i < N_BUFFER; i++)
    {
        buffer_node_t *b = &buf_cache[i];
        b->buf.block_num = BLOCK_NUM_UNUSED;
        b->buf.ref = 0;
        b->buf.data = NULL;
        b->buf.disk = false;
        sleeplock_init(&b->buf.slk, "buffer");

        b->next = buf_head.next;
        b->prev = &buf_head;
        buf_head.next->prev = b;
        buf_head.next = b;
    }

    buf_npages = 0;
}

/* 从buf_cache中获取一个buf (仿照xv6 bget + bread) */
buffer_t *buffer_get(uint32 block_num)
{
    buffer_node_t *b;

    spinlock_acquire(&lk_buf_cache);

    // ========== Step 1: 从MRU向LRU扫描, 查找是否已缓存 ==========
    for (b = buf_head.next; b != &buf_head; b = b->next)
    {
        if (b->buf.block_num == block_num)
        {
            // 命中: 引用计数+1, 位置不动 (xv6 bget不做移动)
            b->buf.ref++;

            // 如果页面被buffer_freemem释放了, 需要重新获取并读盘
            bool need_read = (b->buf.data == NULL);
            if (need_read)
            {
                // 页面被释放的罕见情况: 尝试从LRU端找一个ref==0且有页面的buffer窃取
                buffer_node_t *donor = NULL;
                for (buffer_node_t *n = buf_head.prev; n != &buf_head; n = n->prev)
                {
                    if (n->buf.ref == 0 && n->buf.data != NULL)
                    {
                        donor = n;
                        break;
                    }
                }
                if (donor != NULL)
                {
                    b->buf.data = donor->buf.data;
                    donor->buf.data = NULL;
                }
                else if (buf_npages < N_BUF_PAGES)
                {
                    b->buf.data = (uint8 *)pmem_alloc(true);
                    memset(b->buf.data, 0, BLOCK_SIZE);
                    buf_npages++;
                }
                else
                {
                    panic("buffer_get: no page available (hit path)");
                }
            }

            spinlock_release(&lk_buf_cache);
            sleeplock_acquire(&b->buf.slk);

            if (need_read)
            {
                memset(b->buf.data, 0, BLOCK_SIZE);
                buffer_read(&b->buf);
            }
            return &b->buf;
        }
    }

    // ========== Step 2: 未命中, 从LRU向MRU扫描找ref==0的victim ==========
    // 同时记录第一个 ref==0且有页面 的buffer作为page donor
    buffer_node_t *victim = NULL;
    buffer_node_t *donor = NULL;

    for (b = buf_head.prev; b != &buf_head; b = b->prev)
    {
        if (b->buf.ref == 0)
        {
            if (victim == NULL)
                victim = b;
            if (donor == NULL && b->buf.data != NULL)
                donor = b;
            if (victim != NULL && donor != NULL)
                break;
        }
    }

    if (victim == NULL)
        panic("buffer_get: no buffers");

    // 将victim从当前位置摘除, 插入MRU端 (head->next)
    victim->next->prev = victim->prev;
    victim->prev->next = victim->next;
    victim->next = buf_head.next;
    victim->prev = &buf_head;
    buf_head.next->prev = victim;
    buf_head.next = victim;

    victim->buf.block_num = block_num;
    victim->buf.ref = 1;

    // ========== Step 3: 为victim准备数据页 ==========
    if (victim->buf.data == NULL)
    {
        // victim没有页面, 需要获取
        if (donor != NULL)
        {
            // 从donor窃取页面 (donor是ref==0且有页面的buffer, 且donor != victim)
            victim->buf.data = donor->buf.data;
            donor->buf.data = NULL;
        }
        else if (buf_npages < N_BUF_PAGES)
        {
            // 还有配额, 分配新页面
            victim->buf.data = (uint8 *)pmem_alloc(true);
            memset(victim->buf.data, 0, BLOCK_SIZE);
            buf_npages++;
        }
        else
        {
            // 配额用尽且无donor: 所有页面都在活跃buffer(ref>0)中
            // 这种情况极少发生 (需要768个buffer同时活跃)
            panic("buffer_get: all pages busy");
        }
    }
    // else: victim自带数据页, 复用即可

    spinlock_release(&lk_buf_cache);
    sleeplock_acquire(&victim->buf.slk);

    // 从磁盘读入目标block (覆盖victim->data原有内容)
    buffer_read(&victim->buf);

    return &victim->buf;
}

/* 向buf_cache归还一个buf (仿照xv6 brelse) */
void buffer_put(buffer_t *buf)
{
    sleeplock_release(&buf->slk);

    spinlock_acquire(&lk_buf_cache);

    buf->ref--;
    if (buf->ref == 0)
    {
        // ref归零: 移到MRU端 (head->next), 表示"最近使用过"
        buffer_node_t *b = (buffer_node_t *)buf;

        // 从当前位置摘除
        b->next->prev = b->prev;
        b->prev->next = b->next;

        // 插入head->next (MRU)
        b->next = buf_head.next;
        b->prev = &buf_head;
        buf_head.next->prev = b;
        buf_head.next = b;
    }

    spinlock_release(&lk_buf_cache);
}

/*
    释放非活跃(ref==0)buffer持有的数据页
    从LRU端向MRU端扫描, 释放buffer_count个页面
    返回成功释放的数量
*/
uint32 buffer_freemem(uint32 buffer_count)
{
    uint32 freed = 0;

    spinlock_acquire(&lk_buf_cache);

    // 从LRU端向MRU扫描 (head.prev = 最久未使用)
    for (buffer_node_t *b = buf_head.prev;
         b != &buf_head && freed < buffer_count;)
    {
        buffer_node_t *prev = b->prev; // 保存prev, 因为b可能被移走
        if (b->buf.ref == 0 && b->buf.data != NULL)
        {
            pmem_free((uint64)b->buf.data, true);
            b->buf.data = NULL;
            buf_npages--;
            freed++;
        }
        b = prev;
    }

    spinlock_release(&lk_buf_cache);

    return freed;
}

/* 输出buffer_cache的信息 (for test) */
void buffer_print_info()
{
    buffer_node_t *b;

    assert(N_BUFFER == N_BUFFER_TEST, "buffer_print_info: invalid N_BUFFER");

    spinlock_acquire(&lk_buf_cache);

    printf("buffer_cache information:\n");

    // 单条链表, 分活跃(ref>0)和非活跃(ref==0)两段输出便于阅读
    printf("1.active (ref>0), MRU->LRU:\n");
    for (b = buf_head.next; b != &buf_head; b = b->next)
    {
        if (b->buf.ref == 0)
            break;
        printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
               (int)(b - buf_cache), b->buf.ref, (uint64)b->buf.data, b->buf.block_num);
    }
    printf("over!\n");

    printf("2.inactive (ref==0), MRU->LRU:\n");
    for (b = buf_head.next; b != &buf_head; b = b->next)
    {
        if (b->buf.ref > 0)
            continue;
        printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
               (int)(b - buf_cache), b->buf.ref, (uint64)b->buf.data, b->buf.block_num);
    }
    printf("over!\n");

    spinlock_release(&lk_buf_cache);
}
