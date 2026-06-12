#include "mod.h"

super_block_t sb;          /* 超级块 */
file_t file_table[N_FILE]; // 文件资源池
spinlock_t lk_file_table;  // 保护它的锁

/* 基于superblock输出磁盘布局信息 (for debug) */
static void sb_print()
{
    printf("\ndisk layout information:\n");
    printf("1. super block:  block[0]\n");
    printf("2. inode bitmap: block[%d - %d]\n", sb.inode_bitmap_firstblock,
           sb.inode_bitmap_firstblock + sb.inode_bitmap_blocks - 1);
    printf("3. inode region: block[%d - %d]\n", sb.inode_firstblock,
           sb.inode_firstblock + sb.inode_blocks - 1);
    printf("4. data bitmap:  block[%d - %d]\n", sb.data_bitmap_firstblock,
           sb.data_bitmap_firstblock + sb.data_bitmap_blocks - 1);
    printf("5. data region:  block[%d - %d]\n", sb.data_firstblock,
           sb.data_firstblock + sb.data_blocks - 1);
    printf("block size = %d Byte, total size = %d MB, total inode = %d\n\n", sb.block_size,
           (int)((unsigned long long)(sb.total_blocks) * sb.block_size / 1024 / 1024), sb.total_inodes);
}

/* 文件系统初始化 */
void fs_init()
{
    // 初始化缓冲系统
    buffer_init();

    // 初始化inode cache
    inode_init();

    // 读入超级块
    buffer_t *buf = buffer_get(FS_SB_BLOCK);
    memmove(&sb, buf->data, sizeof(sb));
    buffer_put(buf);

    // 验证魔数
    if (sb.magic_num != FS_MAGIC)
        panic("fs_init: invalid file system magic");

    // 输出磁盘布局信息
    sb_print();

    // 初始化file_table
    file_init();

    // 初始化device_table并创建/dev下的设备文件
    device_init();
}

/* 初始化file_table */
void file_init()
{
    spinlock_init(&lk_file_table, "lk_file_table");
    for (int i = 0; i < N_FILE; i++)
    {
        file_table[i].ip = NULL;
        file_table[i].readable = false;
        file_table[i].writbale = false;
        file_table[i].offset = 0;
        file_table[i].ref = 0;
    }
}

/* 从file_table中获取1个空闲file */
file_t *file_alloc()
{
    spinlock_acquire(&lk_file_table);
    for (int i = 0; i < N_FILE; i++)
    {
        if (file_table[i].ref == 0)
        {
            file_table[i].ref = 1;
            file_table[i].ip = NULL;
            file_table[i].readable = false;
            file_table[i].writbale = false;
            file_table[i].offset = 0;
            spinlock_release(&lk_file_table);
            return &file_table[i];
        }
    }
    spinlock_release(&lk_file_table);
    return NULL;
}

/*
    根据路径打开文件 (指定打开模式)
    成功返回file, 失败返回NULL
*/
file_t *file_open(char *path, uint32 open_mode)
{
    inode_t *ip;

    /* 检查是否需要创建 */
    if (open_mode & FILE_OPEN_CREATE)
    {
        ip = path_to_inode(path);
        if (ip == NULL)
        {
            /* 文件不存在，创建新的 (默认创建 DATA 类型) */
            ip = path_create_inode(path, INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
            if (ip == NULL)
                return NULL;
        }
    }
    else
    {
        ip = path_to_inode(path);
        if (ip == NULL)
            return NULL;
    }

    inode_lock(ip);

    /* 设备文件需要特殊检查 */
    if (ip->disk_info.type == INODE_TYPE_DIVICE)
    {
        if (!device_open_check(ip->disk_info.major, open_mode))
        {
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }
    }

    /* 目录文件只能以只读方式打开 */
    if (ip->disk_info.type == INODE_TYPE_DIR)
    {
        if (open_mode & FILE_OPEN_WRITE)
        {
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }
    }

    inode_unlock(ip);

    /* 分配file结构 */
    file_t *file = file_alloc();
    if (file == NULL)
    {
        inode_put(ip);
        return NULL;
    }

    file->ip = ip;
    file->readable = (open_mode & FILE_OPEN_READ) ? true : false;
    file->writbale = (open_mode & FILE_OPEN_WRITE) ? true : false;
    file->offset = 0;

    return file;
}

/* 关闭文件 */
void file_close(file_t *file)
{
    if (file == NULL)
        return;

    spinlock_acquire(&lk_file_table);
    if (file->ref < 1)
        panic("file_close: ref < 1");

    file->ref--;
    if (file->ref > 0)
    {
        spinlock_release(&lk_file_table);
        return;
    }

    /* ref == 0, 释放资源 */
    inode_t *ip = file->ip;
    file->ip = NULL;
    file->readable = false;
    file->writbale = false;
    file->offset = 0;
    spinlock_release(&lk_file_table);

    /* 释放inode引用 */
    if (ip != NULL)
        inode_put(ip);
}

/* 读取文件内容, 返回读到的字节数量 */
uint32 file_read(file_t *file, uint32 len, uint64 dst, bool is_user_dst)
{
    if (file == NULL || !file->readable)
        return 0;

    inode_t *ip = file->ip;
    if (ip == NULL)
        return 0;

    inode_lock(ip);

    uint32 read_len = 0;

    switch (ip->disk_info.type)
    {
    case INODE_TYPE_DATA:
        /* 流式数据: 从offset开始读 */
        read_len = inode_read_data(ip, file->offset, len, (void *)(uint64)dst, is_user_dst);
        file->offset += read_len;
        break;

    case INODE_TYPE_DIR:
        /* 目录: 传输目录项 */
        read_len = dentry_transmit(ip, dst, len, is_user_dst);
        file->offset += read_len;
        break;

    case INODE_TYPE_DIVICE:
        /* 设备: 调用设备读函数 */
        read_len = device_read_data(ip->disk_info.major, len, dst, is_user_dst);
        /* 设备文件不更新offset */
        break;

    default:
        read_len = 0;
        break;
    }

    inode_unlock(ip);
    return read_len;
}

/* 写入文件内容, 返回写入的字节数量 */
uint32 file_write(file_t *file, uint32 len, uint64 src, bool is_user_src)
{
    if (file == NULL || !file->writbale)
        return 0;

    inode_t *ip = file->ip;
    if (ip == NULL)
        return 0;

    inode_lock(ip);

    uint32 write_len = 0;

    switch (ip->disk_info.type)
    {
    case INODE_TYPE_DATA:
        /* 流式数据: 从offset开始写 */
        write_len = inode_write_data(ip, file->offset, len, (void *)src, is_user_src);
        if (write_len != (uint32)-1)
            file->offset += write_len;
        else
            write_len = 0;
        break;

    case INODE_TYPE_DIR:
        /* 目录不支持直接写入 */
        write_len = 0;
        break;

    case INODE_TYPE_DIVICE:
        /* 设备: 调用设备写函数 */
        write_len = device_write_data(ip->disk_info.major, len, src, is_user_src);
        /* 设备文件不更新offset */
        break;

    default:
        write_len = 0;
        break;
    }

    inode_unlock(ip);
    return write_len;
}

/*
    读/写指针的移动
    对于不合理的lseek_offset, 只做尽力而为的移动
    返回新的file->offset
*/
uint32 file_lseek(file_t *file, uint32 lseek_offset, uint32 lseek_flag)
{
    if (file == NULL || file->ip == NULL)
        return (uint32)-1;

    inode_lock(file->ip);
    uint32 max_size = file->ip->disk_info.size;
    inode_unlock(file->ip);

    uint32 new_offset;

    switch (lseek_flag)
    {
    case FILE_LSEEK_SET:
        new_offset = lseek_offset;
        break;

    case FILE_LSEEK_ADD:
        new_offset = file->offset + lseek_offset;
        break;

    case FILE_LSEEK_SUB:
        if (lseek_offset > file->offset)
            new_offset = 0;
        else
            new_offset = file->offset - lseek_offset;
        break;

    default:
        return (uint32)-1;
    }

    /* 尽力而为: 不能超过文件大小 */
    if (new_offset > max_size)
        new_offset = max_size;

    file->offset = new_offset;
    return file->offset;
}

/* file->ref++ with lock protect */
file_t *file_dup(file_t *file)
{
    if (file == NULL)
        return NULL;

    spinlock_acquire(&lk_file_table);
    if (file->ref < 1)
        panic("file_dup: ref < 1");
    file->ref++;
    spinlock_release(&lk_file_table);
    return file;
}

/* 获取文件参数, 成功返回0, 失败返回-1 */
uint32 file_get_stat(file_t *file, uint64 user_dst)
{
    if (file == NULL || file->ip == NULL)
        return (uint32)-1;

    proc_t *p = myproc();
    file_stat_t stat;

    inode_lock(file->ip);
    stat.type = file->ip->disk_info.type;
    stat.nlink = file->ip->disk_info.nlink;
    stat.size = file->ip->disk_info.size;
    stat.inode_num = file->ip->inode_num;
    inode_unlock(file->ip);

    stat.offset = file->offset;

    uvm_copyout(p->pgtbl, user_dst, (uint64)&stat, sizeof(stat));
    return 0;
}
