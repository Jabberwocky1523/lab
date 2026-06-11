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
        uint64 result = uvm_heap_grow(p->pgtbl, old_heap_top, (uint32)(new_heap_top - old_heap_top));
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
}

/*
    向data_bitmap释放1个block (测试data_bitmap_free)
    uint32 block_num (目标block序号)
    成功返回0
*/
uint64 sys_free_block()
{
}

/*
    从inode_bitmap申请1个inode (测试inode_bitmap_alloc)
    返回block序号
*/
uint64 sys_alloc_inode()
{
}

/*
    向inode_bitmap释放1个inode (测试inode_bitmap_free)
    uint32 inode_num (目标inode序号)
    成功返回0
*/
uint64 sys_free_inode()
{
}

/*
    输出目标bitmap的状态
    uint32 choose_bitmap (0->data_bitmap 1->inode_bitmap)
    成功返回0, 失败返回-1
*/
uint64 sys_show_bitmap()
{
}

/*
    获取1个描述block的buffer (测试buffer_get)
    uint32 block_num 目标block的序号
    成功返回buffer地址, 失败返回-1
*/
uint64 sys_get_block()
{
}

/*
    释放1个描述block的buffer (测试buffer_put)
    uint64 addr_buf 即将被释放的buffer
    成功返回0
*/
uint64 sys_put_block()
{
}

/*
    将buf->data拷贝到用户空间 (测试buffer_read)
    uint64 addr_buf 使用的buffer
    uint64 addr_data 用户数据区 (copy dst)
    成功返回0
*/
uint64 sys_read_block()
{
}

/*
    将用户空间数据同步到内核空间, 并通过buffer写入block (测试buffer_write)
    uint64 addr_buf 使用的buffer
    uint64 addr_data 用户数据区 (copy src)
    成功返回0
*/
uint64 sys_write_block()
{
}

/*
    输出buffer链表的状态
    成功返回0
*/
uint64 sys_show_buffer()
{
}

/*
    释放非活跃链表中buffer持有的物理内存资源
    uint32 buffer_count (希望释放的buffer数量)
    成功返回0
*/
uint64 sys_flush_buffer()
{
}