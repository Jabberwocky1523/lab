#include "mod.h"

static buffer_node_t buf_cache[N_BUFFER];
static buffer_node_t buf_head_active, buf_head_inactive;
static spinlock_t lk_buf_cache;

/* 前向声明 */
static void buffer_read(buffer_t *buf);

/*
    将一个节点拿出来并插入
    1. 活跃链表的头部 buf_head_active->next
    2. 活跃链表的尾部 buf_head_active->prev
    3. 不活跃链表的头部 buf_head_inactive->next
    4. 不活跃链表的尾部 buf_head_inactive->prev
*/
static void insert_node(buffer_node_t *node, bool insert_active, bool insert_next)
{
    /* 如果有需要, 让node先离开当前位置 */
    if (node->next != NULL && node->prev != NULL)
    {
        node->next->prev = node->prev;
        node->prev->next = node->next;
    }

    /* 选择目标双向循环链表 */
    buffer_node_t *head = &buf_head_inactive;
    if (insert_active)
        head = &buf_head_active;

    /* 然后将node插入head->next or head->prev */
    if (insert_next)
    {
        node->next = head->next;
        node->next->prev = node;
        node->prev = head;
        head->next = node;
    }
    else
    {
        node->prev = head->prev;
        node->prev->next = node;
        node->next = head;
        head->prev = node;
    }
}

/*
    buffer系统初始化：
    1. 初始化全局的lk_buf_cache + buf_head_active + buf_head_inactive
    2. 初始化buf_cache中的所有node, 并将他们放在不活跃链表中
*/
void buffer_init()
{
    spinlock_init(&lk_buf_cache, "lk_buf_cache");

    // 初始化链表头
    buf_head_active.next = &buf_head_active;
    buf_head_active.prev = &buf_head_active;
    buf_head_inactive.next = &buf_head_inactive;
    buf_head_inactive.prev = &buf_head_inactive;

    // 初始化所有buffer节点并插入非活跃链表
    for (int i = 0; i < N_BUFFER; i++)
    {
        buffer_node_t *node = &buf_cache[i];
        node->buf.block_num = BLOCK_NUM_UNUSED;
        node->buf.ref = 0;
        node->buf.data = NULL;
        node->buf.disk = false;
        sleeplock_init(&node->buf.slk, "buffer");
        node->next = NULL;
        node->prev = NULL;

        // 插入非活跃链表头部 (head->next)
        insert_node(node, false, true);
    }
}

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

/* 从buf_cache中获取一个buf */
buffer_t *buffer_get(uint32 block_num)
{
    buffer_node_t *node;

    spinlock_acquire(&lk_buf_cache);

    // 1. 首先在活跃链表中寻找 (从head->next开始)
    for (node = buf_head_active.next; node != &buf_head_active; node = node->next)
    {
        if (node->buf.block_num == block_num)
        {
            // 找到后移动到活跃链表的head->next
            insert_node(node, true, true);
            node->buf.ref++;
            spinlock_release(&lk_buf_cache);
            sleeplock_acquire(&node->buf.slk);
            return &node->buf;
        }
    }

    // 2. 在非活跃链表中寻找 (从head->next开始)
    for (node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next)
    {
        if (node->buf.block_num == block_num)
        {
            // 找到后移动到活跃链表的head->next
            insert_node(node, true, true);
            node->buf.ref++;

            bool need_disk_read = false;
            if (node->buf.data == NULL)
            {
                node->buf.data = (uint8 *)pmem_alloc(true);
                memset(node->buf.data, 0, BLOCK_SIZE);
                need_disk_read = true;
            }

            spinlock_release(&lk_buf_cache);
            sleeplock_acquire(&node->buf.slk);

            if (need_disk_read)
                buffer_read(&node->buf);

            return &node->buf;
        }
    }

    // 3. 缓存失败: 将系统中最不活跃的buffer拿出来
    node = buf_head_inactive.prev;
    if (node == &buf_head_inactive)
        panic("buffer_get: no inactive buffer available");

    // 设置block_num, 移动到活跃链表的head->prev
    insert_node(node, true, false);
    node->buf.block_num = block_num;
    node->buf.ref++;

    if (node->buf.data == NULL)
    {
        node->buf.data = (uint8 *)pmem_alloc(true);
        memset(node->buf.data, 0, BLOCK_SIZE);
    }

    spinlock_release(&lk_buf_cache);
    sleeplock_acquire(&node->buf.slk);

    // 从磁盘读入目标block
    buffer_read(&node->buf);

    return &node->buf;
}

/* 向buf_cache归还一个buf */
void buffer_put(buffer_t *buf)
{
    sleeplock_release(&buf->slk);

    spinlock_acquire(&lk_buf_cache);

    buf->ref--;
    if (buf->ref == 0)
    {
        // 移动到不活跃链表的head->next
        buffer_node_t *node = (buffer_node_t *)buf;
        insert_node(node, false, true);
    }

    spinlock_release(&lk_buf_cache);
}

/*
    从后向前遍历非活跃链表, 尝试释放buffer_count个buffer持有的物理内存(data)
    返回成功释放资源的buffer数量
*/
uint32 buffer_freemem(uint32 buffer_count)
{
    uint32 freed = 0;

    spinlock_acquire(&lk_buf_cache);

    // 从后向前遍历非活跃链表 (最不活跃的元素位于head->prev)
    for (buffer_node_t *node = buf_head_inactive.prev;
         node != &buf_head_inactive && freed < buffer_count;)
    {
        if (node->buf.data != NULL)
        {
            pmem_free((uint64)node->buf.data, true);
            node->buf.data = NULL;
            freed++;
        }
        node = node->prev;
    }

    spinlock_release(&lk_buf_cache);

    return freed;
}

/* 输出buffer_cache的信息 (for test) */
void buffer_print_info()
{
    buffer_node_t *node;

    assert(N_BUFFER == N_BUFFER_TEST, "buffer_print_info: invalid N_BUFFER");

    spinlock_acquire(&lk_buf_cache);

    printf("buffer_cache information:\n");

    printf("1.active list:\n");
    for (node = buf_head_active.next; node != &buf_head_active; node = node->next)
    {
        printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
               (int)(node - buf_cache), node->buf.ref, (uint64)node->buf.data, node->buf.block_num);
    }
    printf("over!\n");

    printf("2.inactive list:\n");
    for (node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next)
    {
        printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
               (int)(node - buf_cache), node->buf.ref, (uint64)node->buf.data, node->buf.block_num);
    }
    printf("over!\n");

    spinlock_release(&lk_buf_cache);
}
