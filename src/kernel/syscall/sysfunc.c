#include "mod.h"

#define INT_ARRAY_LEN 5

/*
    测试: 从用户空间传入一个int类型的数组
    uint64 addr 数组起始地址
    uint32 len  元素数量
    成功返回0
*/
uint64 sys_copyin()
{
    proc_t *p = myproc();
    uint64 addr;
    uint32 len;
    arg_uint64(0, &addr);
    arg_uint32(1, &len);

    int arr[INT_ARRAY_LEN];
    if (len > INT_ARRAY_LEN)
        return -1;

    uvm_copyin(p->pgtbl, (uint64)arr, addr, len * sizeof(int));

    printf("sys_copyin: received array from user: ");
    for (uint32 i = 0; i < len; i++)
        printf("%d ", arr[i]);
    printf("\n");

    return 0;
}

/*
    测试: 向用户空间传出一个int类型的数组
    uint64 addr 数组起始地址
    成功返回拷贝的元素数量
*/
uint64 sys_copyout()
{
    proc_t *p = myproc();
    uint64 addr;
    arg_uint64(0, &addr);

    int arr[INT_ARRAY_LEN] = {1, 2, 3, 4, 5};
    uvm_copyout(p->pgtbl, addr, (uint64)arr, INT_ARRAY_LEN * sizeof(int));

    printf("sys_copyout: sent array to user at %p\n", (void *)addr);
    return INT_ARRAY_LEN;
}

/*
    测试: 从用户空间传入一个字符串
    uint64 addr 字符串起始地址
    成功返回0
*/
uint64 sys_copyinstr()
{
    proc_t *p = myproc();
    uint64 addr;
    arg_uint64(0, &addr);

    char buf[STR_MAXLEN + 1];
    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, STR_MAXLEN);

    printf("sys_copyinstr: received string from user: \"%s\"\n", buf);
    return 0;
}

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
uint64 sys_printf()
{
    proc_t *p = myproc();
    uint64 begin;
    uint32 len;
    arg_uint64(0, &begin);
    arg_uint32(1, &len);
    uint32 arr[len];
    uvm_copyin(p->pgtbl, (uint64)arr, begin, len * sizeof(uint32));
    for (uint32 i = 0; i < len; i++)
    {
        printf("%d ", arr[i]);
    }
    return 1;
}