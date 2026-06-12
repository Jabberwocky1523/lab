#include "mod.h"

/*
    用户堆空间伸缩
    uint64 new_heap_top (如果是0, 代表查询当前堆顶位置)
    成功返回new_heap_top, 失败返回-1
*/
uint64 sys_brk()
{
    proc_t *p = myproc();
    uint64 new_heap_top;
    arg_uint64(0, &new_heap_top);

    uint64 old_heap_top = p->heap_top;

    printf("sys_brk: old_heap_top = %p, request new_heap_top = %p\n", (void *)old_heap_top, (void *)new_heap_top);

    // 查询当前堆顶
    if (new_heap_top == 0)
    {
        printf("sys_brk: query heap_top = %p\n", (void *)old_heap_top);
        return old_heap_top;
    }

    if (new_heap_top > old_heap_top)
    {
        // 扩展堆
        uint64 result = uvm_heap_grow(p->pgtbl, old_heap_top, (uint32)(new_heap_top - old_heap_top), PTE_R | PTE_W);
        if (result == old_heap_top)
        {
            // 扩展失败
            printf("sys_brk: heap grow failed\n");
            return -1;
        }
        p->heap_top = result;
        printf("sys_brk: heap grown, new_heap_top = %p\n", (void *)result);
        return result;
    }
    else if (new_heap_top < old_heap_top)
    {
        // 收缩堆
        uint64 result = uvm_heap_ungrow(p->pgtbl, old_heap_top, (uint32)(old_heap_top - new_heap_top));
        p->heap_top = result;
        printf("sys_brk: heap shrunk, new_heap_top = %p\n", (void *)result);
        return result;
    }
    else
    {
        // new_heap_top == old_heap_top, 不变
        printf("sys_brk: heap unchanged at %p\n", (void *)old_heap_top);
        return old_heap_top;
    }
}

/*
    增加一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节,需检查是否是page-aligned)
    成功返回映射空间的起始地址, 失败返回-1
*/
uint64 sys_mmap()
{
    proc_t *p = myproc();
    uint64 begin;
    uint32 len;
    arg_uint64(0, &begin);
    arg_uint32(1, &len);

    printf("sys_mmap: begin = %p, len = %d (0x%x)\n", (void *)begin, len, len);

    // 检查 len 是否页对齐
    if (len % PGSIZE != 0 || len == 0)
    {
        printf("sys_mmap: len not page-aligned or zero\n");
        return -1;
    }

    uint32 npages = len / PGSIZE;

    uint64 actual_begin = uvm_mmap(begin, npages, PTE_R | PTE_W);

    // 调试输出
    printf("sys_mmap: after mmap:\n");
    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");

    return actual_begin;
}

/*
    解除一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节, 需检查是否是page-aligned)
    成功返回0 失败返回-1
*/
uint64 sys_munmap()
{
    proc_t *p = myproc();
    uint64 begin;
    uint32 len;
    arg_uint64(0, &begin);
    arg_uint32(1, &len);

    printf("sys_munmap: begin = %p, len = %d (0x%x)\n", (void *)begin, len, len);

    // 检查 len 是否页对齐
    if (len % PGSIZE != 0 || len == 0)
    {
        printf("sys_munmap: len not page-aligned or zero\n");
        return -1;
    }

    uint32 npages = len / PGSIZE;

    uvm_munmap(begin, npages);

    // 调试输出
    printf("sys_munmap: after munmap:\n");
    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");

    return 0;
}

/*
    打印一个字符串
    char *str
    成功返回0
*/
uint64 sys_print_str()
{
    char buf[STR_MAXLEN + 1];
    arg_str(0, buf, STR_MAXLEN);
    printf("%s", buf);
    return 0;
}

/*
    打印一个32位整数
    int num
    成功返回0
*/
uint64 sys_print_int()
{
    int num;
    arg_uint32(0, (uint32 *)&num);
    printf("%d", num);
    return 0;
}

/*
    进程复制
    返回子进程的pid
*/
uint64 sys_fork()
{
    return proc_fork();
}

/*
    等待子进程退出
    uint64 addr_exit_state
*/
uint64 sys_wait()
{
    uint64 addr;
    arg_uint64(0, &addr);
    return proc_wait(addr);
}

/*
    进程退出
    int exit_code
    不返回
*/
uint64 sys_exit()
{
    int exit_code;
    arg_uint32(0, (uint32 *)&exit_code);
    proc_exit(exit_code);
    return 0; // never reached
}

/*
    让进程睡眠一段时间
    uint32 ntick (1个tick大约0.1秒)
    成功返回0
*/
uint64 sys_sleep()
{
    uint32 ntick;
    arg_uint32(0, &ntick);
    timer_wait(ntick);
    return 0;
}

/*
    返回当前进程的pid
*/
uint64 sys_getpid()
{
    return myproc()->pid;
}

/*
    从data_bitmap申请1个block (测试data_bitmap_alloc)
    返回block序号
*/
uint64 sys_alloc_block()
{
    return bitmap_alloc_block();
}

/*
    向data_bitmap释放1个block (测试data_bitmap_free)
    uint32 block_num (目标block序号)
    成功返回0
*/
uint64 sys_free_block()
{
    uint32 block_num;
    arg_uint32(0, &block_num);
    bitmap_free_block(block_num);
    return 0;
}

/*
    从inode_bitmap申请1个inode (测试inode_bitmap_alloc)
    返回block序号
*/
uint64 sys_alloc_inode()
{
    return bitmap_alloc_inode();
}

/*
    向inode_bitmap释放1个inode (测试inode_bitmap_free)
    uint32 inode_num (目标inode序号)
    成功返回0
*/
uint64 sys_free_inode()
{
    uint32 inode_num;
    arg_uint32(0, &inode_num);
    bitmap_free_inode(inode_num);
    return 0;
}

/*
    输出目标bitmap的状态
    uint32 choose_bitmap (0->data_bitmap 1->inode_bitmap)
    成功返回0, 失败返回-1
*/
uint64 sys_show_bitmap()
{
    uint32 choose;
    arg_uint32(0, &choose);
    if (choose > 1)
        return -1;
    // choose: 0->data_bitmap, 1->inode_bitmap
    bitmap_print(choose == 0);
    return 0;
}

/*
    获取1个描述block的buffer (测试buffer_get)
    uint32 block_num 目标block的序号
    成功返回buffer地址, 失败返回-1
*/
uint64 sys_get_block()
{
    uint32 block_num;
    arg_uint32(0, &block_num);
    buffer_t *buf = buffer_get(block_num);
    return (uint64)buf;
}

/*
    释放1个描述block的buffer (测试buffer_put)
    uint64 addr_buf 即将被释放的buffer
    成功返回0
*/
uint64 sys_put_block()
{
    uint64 addr_buf;
    arg_uint64(0, &addr_buf);
    buffer_put((buffer_t *)addr_buf);
    return 0;
}

/*
    将buf->data拷贝到用户空间 (测试buffer_read)
    uint64 addr_buf 使用的buffer
    uint64 addr_data 用户数据区 (copy dst)
    成功返回0
*/
uint64 sys_read_block()
{
    uint64 addr_buf, addr_data;
    arg_uint64(0, &addr_buf);
    arg_uint64(1, &addr_data);

    buffer_t *buf = (buffer_t *)addr_buf;
    proc_t *p = myproc();

    uvm_copyout(p->pgtbl, addr_data, (uint64)buf->data, BLOCK_SIZE);
    return 0;
}

/*
    将用户空间数据同步到内核空间, 并通过buffer写入block (测试buffer_write)
    uint64 addr_buf 使用的buffer
    uint64 addr_data 用户数据区 (copy src)
    成功返回0
*/
uint64 sys_write_block()
{
    uint64 addr_buf, addr_data;
    arg_uint64(0, &addr_buf);
    arg_uint64(1, &addr_data);

    buffer_t *buf = (buffer_t *)addr_buf;
    proc_t *p = myproc();

    uvm_copyin(p->pgtbl, (uint64)buf->data, addr_data, BLOCK_SIZE);
    buffer_write(buf);
    return 0;
}

/*
    输出buffer链表的状态
    成功返回0
*/
uint64 sys_show_buffer()
{
    buffer_print_info();
    return 0;
}

/*
    释放非活跃链表中buffer持有的物理内存资源
    uint32 buffer_count (希望释放的buffer数量)
    成功返回0
*/
uint64 sys_flush_buffer()
{
    uint32 buffer_count;
    arg_uint32(0, &buffer_count);
    buffer_freemem(buffer_count);
    return 0;
}

/*
    执行ELF文件以替换当前进程的内容
    char *path
    char **argv
    成功返回argc, 失败返回-1
*/
uint64 sys_exec()
{
    char path[STR_MAXLEN + 1];
    char *argv[ELF_MAXARGS + 1];
    uint64 uargv;
    int i;

    /* 读取path参数 */
    arg_str(0, path, STR_MAXLEN);

    /* 读取argv数组 (用户空间中的指针数组) */
    arg_uint64(1, &uargv);

    memset(argv, 0, sizeof(argv));

    /* 从用户空间逐个读取argv字符串 */
    for (i = 0; i < ELF_MAXARGS; i++)
    {
        uint64 uarg_ptr;
        proc_t *p = myproc();

        /* 读取argv[i]指针 */
        uvm_copyin(p->pgtbl, (uint64)&uarg_ptr, uargv + i * sizeof(uint64), sizeof(uint64));

        if (uarg_ptr == 0)
            break;

        /* 分配内核空间暂存字符串 */
        argv[i] = (char *)pmem_alloc(true);
        if (argv[i] == NULL)
        {
            /* 释放之前分配的 */
            for (int j = 0; j < i; j++)
                pmem_free((uint64)argv[j], true);
            return -1;
        }

        uvm_copyin_str(p->pgtbl, (uint64)argv[i], uarg_ptr, ELF_MAXARG_LEN);
    }

    int ret = proc_exec(path, argv);

    /* 释放临时分配的内存 */
    for (int j = 0; j < i; j++)
        pmem_free((uint64)argv[j], true);

    return ret;
}

/* 构建fd->file的映射, 返回fd */
static uint32 alloc_fd(file_t *file)
{
    proc_t *p = myproc();
    for (uint32 i = 0; i < N_OPEN_FILE_PER_PROC; i++)
    {
        if (p->open_file[i] == NULL)
        {
            p->open_file[i] = file;
            return i;
        }
    }
    return -1;
}

/*
    打开或创建文件
    char *path
    uint32 open_mode
    成功返回fd, 失败返回-1
*/
uint64 sys_open()
{
    char path[STR_MAXLEN + 1];
    uint32 open_mode;

    arg_str(0, path, STR_MAXLEN);
    arg_uint32(1, &open_mode);

    file_t *file = file_open(path, open_mode);
    if (file == NULL)
        return -1;

    uint32 fd = alloc_fd(file);
    if (fd == (uint32)-1)
    {
        file_close(file);
        return -1;
    }

    return fd;
}

/*
    关闭文件
    uint32 fd
    成功返回0, 失败返回-1
*/
uint64 sys_close()
{
    proc_t *p = myproc();
    uint32 fd;

    arg_uint32(0, &fd);

    if (fd >= N_OPEN_FILE_PER_PROC || p->open_file[fd] == NULL)
        return -1;

    file_close(p->open_file[fd]);
    p->open_file[fd] = NULL;
    return 0;
}

/*
    读取文件内容
    uint32 fd
    uint32 len
    uint64 addr
    成功返回读到的字节数, 失败返回0
*/
uint64 sys_read()
{
    proc_t *p = myproc();
    uint32 fd, len;
    uint64 addr;

    arg_uint32(0, &fd);
    arg_uint32(1, &len);
    arg_uint64(2, &addr);

    if (fd >= N_OPEN_FILE_PER_PROC || p->open_file[fd] == NULL)
        return 0;

    return file_read(p->open_file[fd], len, addr, true);
}

/*
    写入文件内容
    uint32 fd
    uint32 len
    uint64 addr
    成功返回写入的字节数, 失败返回0
*/
uint64 sys_write()
{
    proc_t *p = myproc();
    uint32 fd, len;
    uint64 addr;

    arg_uint32(0, &fd);
    arg_uint32(1, &len);
    arg_uint64(2, &addr);

    if (fd >= N_OPEN_FILE_PER_PROC || p->open_file[fd] == NULL)
        return 0;

    return file_write(p->open_file[fd], len, addr, true);
}

/*
    调整读写指针位置
    uint32 fd
    uint32 offset
    uint32 flag
    成功返回新的偏移量, 失败返回-1
*/
uint64 sys_lseek()
{
    proc_t *p = myproc();
    uint32 fd, offset, flag;

    arg_uint32(0, &fd);
    arg_uint32(1, &offset);
    arg_uint32(2, &flag);

    if (fd >= N_OPEN_FILE_PER_PROC || p->open_file[fd] == NULL)
        return -1;

    return file_lseek(p->open_file[fd], offset, flag);
}

/*
    复制文件控制权
    uinr32 fd
    成功返回new_fd, 失败返回-1
*/
uint64 sys_dup()
{
    proc_t *p = myproc();
    uint32 fd;

    arg_uint32(0, &fd);

    if (fd >= N_OPEN_FILE_PER_PROC || p->open_file[fd] == NULL)
        return -1;

    file_t *f = file_dup(p->open_file[fd]);
    if (f == NULL)
        return -1;

    uint32 new_fd = alloc_fd(f);
    if (new_fd == (uint32)-1)
    {
        file_close(f);
        return -1;
    }

    return new_fd;
}

/*
    获取文件信息
    uint32 fd
    uint64 addr
    成功返回0, 失败返回-1
*/
uint64 sys_fstat()
{
    proc_t *p = myproc();
    uint32 fd;
    uint64 addr;

    arg_uint32(0, &fd);
    arg_uint64(1, &addr);

    if (fd >= N_OPEN_FILE_PER_PROC || p->open_file[fd] == NULL)
        return -1;

    return file_get_stat(p->open_file[fd], addr);
}

/*
    获取目录中的所有目录项信息
    uint32 fd
    uint64 addr
    uint32 buffer_len
    成功返回读到的字节数, 失败返回-1
*/
uint64 sys_get_dentries()
{
    proc_t *p = myproc();
    uint32 fd, buf_len;
    uint64 addr;

    arg_uint32(0, &fd);
    arg_uint64(1, &addr);
    arg_uint32(2, &buf_len);

    if (fd >= N_OPEN_FILE_PER_PROC || p->open_file[fd] == NULL)
        return -1;

    /* file_read 对于目录文件会调用 dentry_transmit */
    uint32 read_len = file_read(p->open_file[fd], buf_len, addr, true);

    /* 目录读完后重置offset以便下次读取 */
    p->open_file[fd]->offset = 0;

    return read_len;
}

/*
    创建目录
    char *path
    成功返回0, 失败返回-1
*/
uint64 sys_mkdir()
{
    char path[STR_MAXLEN + 1];

    arg_str(0, path, STR_MAXLEN);

    inode_t *ip = path_create_inode(path, INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    if (ip == NULL)
        return -1;

    inode_put(ip);
    return 0;
}

/*
    修改当前工作目录
    char *new_path
    成功返回0, 失败返回-1
*/
uint64 sys_chdir()
{
    proc_t *p = myproc();
    char new_path[STR_MAXLEN + 1];

    arg_str(0, new_path, STR_MAXLEN);

    inode_t *ip = path_to_inode(new_path);
    if (ip == NULL)
        return -1;

    inode_lock(ip);

    /* 必须是目录 */
    if (ip->disk_info.type != INODE_TYPE_DIR)
    {
        inode_unlock(ip);
        inode_put(ip);
        return -1;
    }

    inode_unlock(ip);

    /* 替换当前工作目录 */
    if (p->cwd != NULL)
        inode_put(p->cwd);
    p->cwd = ip; /* ip的ref已经被path_to_inode增加了 */

    return 0;
}

/*
    打印当前工作目录的绝对路径
    成功返回0, 失败返回-1
*/
uint64 sys_print_cwd()
{
    proc_t *p = myproc();

    if (p->cwd == NULL)
    {
        printf("cwd: (null)\n");
        return -1;
    }

    /* 分配缓冲区用于存储路径 */
    char path[STR_MAXLEN + 1];
    uint32 offset = inode_to_path(p->cwd, path, STR_MAXLEN);
    if (offset == (uint32)-1)
    {
        printf("cwd: (error)\n");
        return -1;
    }

    printf("cwd: %s\n", path + offset);
    return 0;
}

/*
    新建链接
    char *old_path
    char *new_path
    成功返回0, 失败返回-1
*/
uint64 sys_link()
{
    char old_path[STR_MAXLEN + 1];
    char new_path[STR_MAXLEN + 1];

    arg_str(0, old_path, STR_MAXLEN);
    arg_str(1, new_path, STR_MAXLEN);

    return path_link(old_path, new_path);
}

/*
    删除链接 (可能触发删除文件)
    char *path
    成功返回0, 失败返回-1
*/
uint64 sys_unlink()
{
    char path[STR_MAXLEN + 1];

    arg_str(0, path, STR_MAXLEN);

    return path_unlink(path);
}