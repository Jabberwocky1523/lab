#include "mod.h"

extern super_block_t sb;

/* 内存中的inode资源集合 */
static inode_t inode_cache[N_INODE];
static spinlock_t lk_inode_cache;

/* inode_cache初始化 */
void inode_init()
{
    spinlock_init(&lk_inode_cache, "lk_inode_cache");
    for (int i = 0; i < N_INODE; i++)
    {
        inode_cache[i].valid_info = false;
        inode_cache[i].ref = 0;
        inode_cache[i].inode_num = INVALID_INODE_NUM;
        sleeplock_init(&inode_cache[i].slk, "inode");
    }
}

/*--------------------关于inode->index的增删查操作-----------------*/

/*
    供free_data_blocks使用
    递归删除inode->index中的一个元素
    返回删除过程中是否遇到空的block_num (文件末尾)
*/
static bool __free_data_blocks(uint32 block_num, uint32 level)
{
    /* 遇到空的block_num说明到达文件末尾 */
    if (block_num == 0)
        return true;

    /* level==0: 直接释放数据块 */
    if (level == 0)
    {
        bitmap_free_block(block_num);
        return false;
    }

    /* level > 0: 读入索引块，遍历其entry */
    buffer_t *buf = buffer_get(block_num);
    uint32 *index_block = (uint32 *)buf->data;
    uint32 entry_count = BLOCK_SIZE / sizeof(uint32);
    bool meet_empty = false;

    for (uint32 i = 0; i < entry_count; i++)
    {
        if (index_block[i] != 0)
        {
            meet_empty = __free_data_blocks(index_block[i], level - 1);
            if (meet_empty)
                break;
        }
        else
        {
            /* index_block[i] == 0: 到达文件末尾 */
            meet_empty = true;
            break;
        }
    }

    buffer_put(buf);

    /* 释放索引块本身 */
    bitmap_free_block(block_num);

    return meet_empty;
}

/*
    释放inode管理的blocks
*/
static void free_data_blocks(uint32 *inode_index)
{
    unsigned int i;
    bool meet_empty = false;

    /* step-1: 释放直接映射的block */
    for (i = 0; i < INODE_INDEX_1; i++)
    {
        meet_empty = __free_data_blocks(inode_index[i], 0);
        if (meet_empty)
            return;
    }

    /* step-2: 释放一级间接映射的block */
    for (; i < INODE_INDEX_2; i++)
    {
        meet_empty = __free_data_blocks(inode_index[i], 1);
        if (meet_empty)
            return;
    }

    /* step-3: 释放二级间接映射的block */
    for (; i < INODE_INDEX_3; i++)
    {
        meet_empty = __free_data_blocks(inode_index[i], 2);
        if (meet_empty)
            return;
    }

    panic("free_data_blocks: impossible!");
}

/*
    获取inode第logical_block_num个block的物理序号block_num
    调用者保证输入的logical_block_num只有两种情况:
    1. 属于已经分配的区域 (返回block_num)
    2. 将已经分配出去的区域往外扩展1个block (申请block并返回block_num)
    成功返回block_num, 失败返回-1
*/
static uint32 locate_or_add_block(uint32 *inode_index, uint32 logical_block_num)
{
    uint32 entry_per_block = BLOCK_SIZE / sizeof(uint32);

    /* Case 1: 直接映射 index[0..9] */
    if (logical_block_num < INODE_INDEX_1)
    {
        if (inode_index[logical_block_num] == 0)
            inode_index[logical_block_num] = bitmap_alloc_block();
        return inode_index[logical_block_num];
    }

    /* Case 2: 一级间接映射 index[10..11] */
    if (logical_block_num < INODE_BLOCK_INDEX_2)
    {
        uint32 remaining = logical_block_num - INODE_INDEX_1;
        uint32 idx_slot = INODE_INDEX_1 + remaining / entry_per_block;
        uint32 idx_off = remaining % entry_per_block;

        /* 分配一级索引块(如果不存在) */
        if (inode_index[idx_slot] == 0)
            inode_index[idx_slot] = bitmap_alloc_block();

        /* 读入一级索引块 */
        buffer_t *buf = buffer_get(inode_index[idx_slot]);
        uint32 *idx_block = (uint32 *)buf->data;

        /* 分配数据块(如果不存在) */
        if (idx_block[idx_off] == 0)
        {
            idx_block[idx_off] = bitmap_alloc_block();
            buffer_write(buf);
        }

        uint32 result = idx_block[idx_off];
        buffer_put(buf);
        return result;
    }

    /* Case 3: 二级间接映射 index[12] */
    if (logical_block_num < INODE_BLOCK_INDEX_3)
    {
        uint32 remaining = logical_block_num - INODE_BLOCK_INDEX_2;
        uint32 idx_slot = INODE_INDEX_2; /* index[12] */
        uint32 idx_l1 = remaining / entry_per_block;
        uint32 idx_l0 = remaining % entry_per_block;

        /* 分配二级索引块(如果不存在) */
        if (inode_index[idx_slot] == 0)
            inode_index[idx_slot] = bitmap_alloc_block();

        /* 读入二级索引块 */
        buffer_t *buf2 = buffer_get(inode_index[idx_slot]);
        uint32 *idx2 = (uint32 *)buf2->data;

        /* 分配一级索引块(如果不存在) */
        if (idx2[idx_l1] == 0)
        {
            idx2[idx_l1] = bitmap_alloc_block();
            buffer_write(buf2);
        }

        uint32 level1_block = idx2[idx_l1];
        buffer_put(buf2);

        /* 读入一级索引块 */
        buffer_t *buf1 = buffer_get(level1_block);
        uint32 *idx1 = (uint32 *)buf1->data;

        /* 分配数据块(如果不存在) */
        if (idx1[idx_l0] == 0)
        {
            idx1[idx_l0] = bitmap_alloc_block();
            buffer_write(buf1);
        }

        uint32 result = idx1[idx_l0];
        buffer_put(buf1);
        return result;
    }

    panic("locate_or_add_block: out of range");
    return (uint32)-1;
}

/*---------------------关于inode的管理: get dup lock unlock put----------------------*/

/*
    磁盘里的inode <-> 内存里的inode
    调用者需要持有ip->slk并设置合理的inode_num
*/
void inode_rw(inode_t *ip, bool write)
{
    /* 计算inode在磁盘inode_region中的位置 */
    uint32 block_num = sb.inode_firstblock + ip->inode_num / INODE_PER_BLOCK;
    uint32 byte_offset = (ip->inode_num % INODE_PER_BLOCK) * sizeof(inode_disk_t);

    buffer_t *buf = buffer_get(block_num);
    inode_disk_t *dip = (inode_disk_t *)(buf->data + byte_offset);

    if (write)
    {
        /* 内存 -> 磁盘 */
        memmove(dip, &ip->disk_info, sizeof(inode_disk_t));
        buffer_write(buf);
    }
    else
    {
        /* 磁盘 -> 内存 */
        memmove(&ip->disk_info, dip, sizeof(inode_disk_t));
    }

    buffer_put(buf);
}

/*
    尝试在inode_cache里寻找是否存在目标inode
    如果不存在则申请一个空闲的inode
    如果没有空闲位置直接panic
    核心逻辑: ref++
*/
inode_t *inode_get(uint32 inode_num)
{
    spinlock_acquire(&lk_inode_cache);

    /* 1. 查找是否已在cache中 */
    inode_t *empty = NULL;
    for (int i = 0; i < N_INODE; i++)
    {
        if (inode_cache[i].ref > 0 && inode_cache[i].inode_num == inode_num)
        {
            inode_cache[i].ref++;
            spinlock_release(&lk_inode_cache);
            return &inode_cache[i];
        }
        if (empty == NULL && inode_cache[i].ref == 0)
            empty = &inode_cache[i];
    }

    /* 2. 使用空闲槽位 */
    if (empty == NULL)
        panic("inode_get: no free inode");

    /* 3. 初始化空闲槽位 (disk_info待inode_lock时从磁盘读取) */
    empty->inode_num = inode_num;
    empty->ref = 1;
    empty->valid_info = false;
    spinlock_release(&lk_inode_cache);

    return empty;
}

/*
    在磁盘里创建1个新的inode
    1. 查询和修改inode_bitmap
    2. 填充inode_region对应位置的inode
    注意: 返回的inode未上锁
*/
inode_t *inode_create(uint16 type, uint16 major, uint16 minor)
{
    /* 1. 分配inode_num */
    uint32 inode_num = bitmap_alloc_inode();

    /* 2. 获取inode cache槽位 */
    inode_t *ip = inode_get(inode_num);

    /* 3. 填充disk_info并写回磁盘 */
    sleeplock_acquire(&ip->slk);
    ip->disk_info.type = type;
    ip->disk_info.major = major;
    ip->disk_info.minor = minor;
    ip->disk_info.nlink = 1;
    ip->disk_info.size = 0;
    for (int i = 0; i < INODE_INDEX_3; i++)
        ip->disk_info.index[i] = 0;
    ip->valid_info = true;

    inode_rw(ip, true);
    sleeplock_release(&ip->slk);

    return ip;
}

/*
    ip->ref++ with lock proctect
*/
inode_t *inode_dup(inode_t *ip)
{
    spinlock_acquire(&lk_inode_cache);
    ip->ref++;
    spinlock_release(&lk_inode_cache);
    return ip;
}

/*
    锁住inode
    如果inode->disk_info无效则从磁盘读取
*/
void inode_lock(inode_t *ip)
{
    if (ip == NULL || ip->ref < 1)
        panic("inode_lock");

    sleeplock_acquire(&ip->slk);

    /* disk_info无效则从磁盘读入 */
    if (ip->valid_info == false)
    {
        inode_rw(ip, false);
        ip->valid_info = true;
        if (ip->disk_info.type == 0 && ip->disk_info.nlink == 0)
            panic("inode_lock: no type");
    }
}

/*
    解锁inode
*/
void inode_unlock(inode_t *ip)
{
    if (ip == NULL || !sleeplock_holding(&ip->slk) || ip->ref < 1)
        panic("inode_unlock");

    sleeplock_release(&ip->slk);
}

/*
    与inode_get相对应, 调用者释放inode资源
    如果达成某些条件, 可能触发彻底删除
*/
void inode_put(inode_t *ip)
{
    spinlock_acquire(&lk_inode_cache);

    /* 如果这是最后一个引用且nlink为0, 彻底删除 */
    if (ip->ref == 1 && ip->valid_info && ip->disk_info.nlink == 0)
    {
        /* ip->ref == 1 意味着没有其他竞争者, acquiresleep不会阻塞 */
        sleeplock_acquire(&ip->slk);
        spinlock_release(&lk_inode_cache);

        inode_delete(ip);
        ip->disk_info.type = 0;
        ip->disk_info.nlink = 0;
        inode_rw(ip, true);
        ip->valid_info = false;

        sleeplock_release(&ip->slk);

        spinlock_acquire(&lk_inode_cache);
    }

    ip->ref--;
    spinlock_release(&lk_inode_cache);
}

/*
    在磁盘里删除1个inode
    1. 修改inode_bitmap释放inode_region资源
    2. 修改block_bitmap释放block_region资源
    注意: 调用者需要持有ip->slk
*/
void inode_delete(inode_t *ip)
{
    /* 释放所有数据块 */
    free_data_blocks(ip->disk_info.index);
    ip->disk_info.size = 0;

    /* 释放inode在bitmap中的占用 */
    bitmap_free_inode(ip->inode_num);
}

/*----------------------基于inode的数据读写操作--------------------*/

/*
    基于inode的数据读取
    inode管理的数据空间逻辑上是一个连续的数组data
    需要拷贝data[offset,offset+len)到dst(用户态地址/内核态地址)
    返回读取的数据量(字节)
*/
uint32 inode_read_data(inode_t *ip, uint32 offset, uint32 len, void *dst, bool is_user_dst)
{
    assert(sleeplock_holding(&ip->slk), "inode_read_data: slk");

    /* 不能超出文件大小 */
    if (offset > ip->disk_info.size)
        return 0;
    if (offset + len > ip->disk_info.size)
        len = ip->disk_info.size - offset;

    uint32 total = 0;
    char *dst_bytes = (char *)dst;

    while (total < len)
    {
        uint32 block_num = offset / BLOCK_SIZE;
        uint32 block_off = offset % BLOCK_SIZE;
        uint32 chunk = len - total;
        if (chunk > BLOCK_SIZE - block_off)
            chunk = BLOCK_SIZE - block_off;

        uint32 phys_block = locate_or_add_block(ip->disk_info.index, block_num);
        buffer_t *buf = buffer_get(phys_block);

        memmove(dst_bytes + total, buf->data + block_off, chunk);

        buffer_put(buf);

        total += chunk;
        offset += chunk;
    }

    return total;
}

/*
    基于inode的数据写入
    inode管理的数据空间逻辑上是一个连续的数组data
    需要拷贝src(用户态地址/内核态地址)到data[offset,offset+len)
    返回写入的数据量(字节)
*/
uint32 inode_write_data(inode_t *ip, uint32 offset, uint32 len, void *src, bool is_user_src)
{
    assert(sleeplock_holding(&ip->slk), "inode_write_data: slk");

    /* offset不能超过当前文件大小 (无空洞) */
    if (offset > ip->disk_info.size)
        return 0;

    /* 不能超过文件上限 */
    if ((uint64)offset + len > INODE_MAX_SIZE)
        return (uint32)-1;

    uint32 total = 0;
    char *src_bytes = (char *)src;

    while (total < len)
    {
        uint32 block_num = offset / BLOCK_SIZE;
        uint32 block_off = offset % BLOCK_SIZE;
        uint32 chunk = len - total;
        if (chunk > BLOCK_SIZE - block_off)
            chunk = BLOCK_SIZE - block_off;

        uint32 phys_block = locate_or_add_block(ip->disk_info.index, block_num);
        buffer_t *buf = buffer_get(phys_block);

        memmove(buf->data + block_off, src_bytes + total, chunk);
        buffer_write(buf);
        buffer_put(buf);

        total += chunk;
        offset += chunk;
    }
    
    /* 更新文件大小 */
    if (offset > ip->disk_info.size)
        ip->disk_info.size = offset;

    return total;
}

static char *inode_type_list[] = {"DATA", "DIR", "DEVICE"};

/* 输出inode信息(for debug) */
void inode_print(inode_t *ip, char *name)
{
    assert(sleeplock_holding(&ip->slk), "inode_print: slk");

    spinlock_acquire(&lk_inode_cache);

    printf("inode %s:\n", name);
    printf("ref = %d, inode_num = %d, valid_info = %d\n", ip->ref, ip->inode_num, ip->valid_info);
    printf("type = %s, major = %d, minor = %d, nlink = %d, size = %d\n", inode_type_list[ip->disk_info.type],
           ip->disk_info.major, ip->disk_info.minor, ip->disk_info.nlink, ip->disk_info.size);

    printf("index_list = [ ");
    for (int i = 0; i < INODE_INDEX_1; i++)
        printf("%d ", ip->disk_info.index[i]);
    printf("] [ ");
    for (int i = INODE_INDEX_1; i < INODE_INDEX_2; i++)
        printf("%d ", ip->disk_info.index[i]);
    printf("] [ ");
    for (int i = INODE_INDEX_2; i < INODE_INDEX_3; i++)
        printf("%d ", ip->disk_info.index[i]);
    printf("]\n\n");

    spinlock_release(&lk_inode_cache);
}
