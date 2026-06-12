#include "mod.h"

/* 外部声明 */
extern char trampoline[];

/*
    将ELF文件中的segment放入内存中制定位置
    inode逻辑区域: [seg_start, seg_start + len)
    进程地址空间: [va_start, va_start + len), 对应的物理页是存在的
*/
static void load_segment(inode_t *ip, pgtbl_t pgtbl,
                         uint64 seg_start, uint64 va_start, uint32 len)
{
    assert(va_start % PGSIZE == 0, "load_segment: va aligned!");

    pte_t *pte;
    uint64 pa;
    uint32 read_len, cut_len;

    for (read_len = 0; read_len < len; read_len += PGSIZE)
    {
        /* 获取物理内存地址 */
        pte = vm_getpte(pgtbl, va_start + read_len, false);
        pa = PTE_TO_PA(*pte);
        assert(pa != 0, "load_segment: invalid pa!");

        /* 读入segment的一部分 */
        cut_len = MIN(len - read_len, PGSIZE);
        if (inode_read_data(ip, (uint32)seg_start + read_len, cut_len, (void *)pa, false) != cut_len)
            panic("load_segment: read fail!");
    }
}

/* 将程序的代码区和数据区读入用户堆中, 返回new_heap_top */
static uint64 prepare_heap(pgtbl_t new_pgtbl, inode_t *ip, elf_header_t *eh)
{
    program_header_t ph;
    uint64 new_heap_top = USER_BASE, old_heap_top = USER_BASE;

    for (uint32 off = eh->ph_off; off < eh->ph_off + eh->ph_ent_num * sizeof(ph); off += sizeof(ph))
    {
        // 读入一个program header
        if (inode_read_data(ip, off, sizeof(ph), &ph, false) != sizeof(ph))
            return -1;

        // 判断是否有必要载入
        if (ph.type != ELF_PROG_LOAD)
            continue;

        // program header参数的合法性检查
        if (ph.mem_size < ph.file_size)
            return -1;
        if (ph.va + ph.mem_size < ph.va)
            return -1;
        if (ph.va % PGSIZE != 0)
            return -1;

        // 用户堆生长
        new_heap_top = uvm_heap_grow(new_pgtbl, old_heap_top,
                                     ph.va + ph.mem_size - old_heap_top, PTE_R | PTE_X);
        if (new_heap_top != ph.va + ph.mem_size)
            return -1;
        old_heap_top = new_heap_top;

        // segment读入
        load_segment(ip, new_pgtbl, ph.off, ph.va, ph.file_size);
    }

    return new_heap_top;
}

/* 准备栈空间用于存储输入参数(4KB), 设置arg_count, 返回sp */
static uint64 prepare_stack(pgtbl_t new_pgtbl, char **argv, int *arg_count)
{
    uint64 ustack_page;
    uint64 sp = TRAPFRAME, sp_base = TRAPFRAME - PGSIZE;
    uint64 sp_list[ELF_MAXARGS + 1];
    uint32 argc, arg_len;

    ustack_page = (uint64)pmem_alloc(false);
    vm_mappages(new_pgtbl, sp_base, ustack_page, PGSIZE, PTE_R | PTE_W | PTE_U);

    for (argc = 0; argv[argc] != NULL; argc++)
    {
        if (argc >= ELF_MAXARGS)
            return -1;

        arg_len = strlen(argv[argc]) + 1;
        sp -= ALIGN_UP(arg_len, 16);
        if (sp < sp_base)
            return -1;

        uvm_copyout(new_pgtbl, sp, (uint64)argv[argc], arg_len);

        sp_list[argc] = sp;
    }
    sp_list[argc] = 0;

    arg_len = (argc + 1) * sizeof(uint64);
    sp -= ALIGN_UP(arg_len, 16);
    if (sp < sp_base)
        return -1;

    uvm_copyout(new_pgtbl, sp, (uint64)sp_list, arg_len);

    *arg_count = argc;

    return sp;
}

/*
    执行ELF文件
    输入路径和参数
    成功返回argc, 失败返回-1
*/
int proc_exec(char *path, char **argv)
{
    proc_t *p = myproc();
    inode_t *ip = NULL;
    elf_header_t eh;
    pgtbl_t new_pgtbl = NULL;
    trapframe_t *new_tf = NULL;
    uint64 new_heap_top;
    uint64 sp;
    int argc;

    /* step-0: 准备全新的pagetable和trapframe */
    new_tf = (trapframe_t *)pmem_alloc(true);
    if (new_tf == NULL)
        return -1;
    memset(new_tf, 0, PGSIZE);

    new_pgtbl = (pgtbl_t)pmem_alloc(true);
    if (new_pgtbl == NULL)
    {
        pmem_free((uint64)new_tf, true);
        return -1;
    }
    memset(new_pgtbl, 0, PGSIZE);

    /* 映射trampoline和trapframe */
    vm_mappages(new_pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    vm_mappages(new_pgtbl, TRAPFRAME, (uint64)new_tf, PGSIZE, PTE_R | PTE_W);

    /* step-1: 解析输入的文件路径, 获取ELF文件的inode */
    ip = path_to_inode(path);
    if (ip == NULL)
        goto bad;

    inode_lock(ip);

    /* step-2: 读取ELF_header */
    if (inode_read_data(ip, 0, sizeof(eh), &eh, false) != sizeof(eh))
        goto bad;

    /* 验证ELF magic */
    if (eh.magic != ELF_MAGIC)
        goto bad;

    /* step-3: 按照顺序读取需要载入内存的Segment, 填充到用户堆区域 */
    new_heap_top = prepare_heap(new_pgtbl, ip, &eh);
    if (new_heap_top == (uint64)-1)
        goto bad;

    /* step-4: 释放ELF的inode */
    inode_unlock(ip);
    inode_put(ip);
    ip = NULL;

    /* step-5: 处理输入的参数列表argv, 填充到用户栈区域 */
    sp = prepare_stack(new_pgtbl, argv, &argc);
    if (sp == (uint64)-1)
        goto bad;

    /* step-6: 释放旧的资源 */
    /* 先释放旧页表 */
    if (p->pgtbl != NULL)
    {
        uvm_destroy_pgtbl(p->pgtbl);
        p->pgtbl = NULL;
    }

    /* 释放旧的trapframe (旧页表释放时已处理TRAPFRAME映射，但物理页需要单独释放？) */
    /* uvm_destroy_pgtbl 已经处理了 TRAPFRAME 的释放, 所以 p->tf 对应的物理页已被释放 */
    p->tf = NULL;

    /* 释放旧的mmap链表 */
    mmap_region_t *r = p->mmap;
    while (r != NULL)
    {
        mmap_region_t *next = r->next;
        mmap_region_free(r);
        r = next;
    }
    p->mmap = NULL;

    /* step-7: 设置trapframe的相关字段 */
    /* 将内核信息填入新的trapframe */
    new_tf->user_to_kern_satp = p->tf ? p->tf->user_to_kern_satp : 0;
    new_tf->user_to_kern_sp = p->kstack + PGSIZE;
    new_tf->user_to_kern_trapvector = p->tf ? p->tf->user_to_kern_trapvector : 0;
    new_tf->user_to_kern_hartid = p->tf ? p->tf->user_to_kern_hartid : 0;

    new_tf->user_to_kern_epc = eh.entry;  /* 用户程序入口地址 */
    new_tf->sp = sp;                      /* 用户栈指针 */
    new_tf->a0 = argc;                    /* main的第一个参数 */
    new_tf->a1 = sp;                      /* main的第二个参数 (argv在栈上的地址) */

    /* step-8: 更新进程的相关字段 */
    p->pgtbl = new_pgtbl;
    p->tf = new_tf;
    p->heap_top = new_heap_top;
    p->ustack_npage = 1;  /* prepare_stack分配了1页栈空间 */

    /* 更新进程名称 (从路径提取文件名) */
    const char *last = path;
    for (const char *s = path; *s; s++)
        if (*s == '/')
            last = s + 1;
    int name_len = strlen(last);
    name_len = MIN(name_len, 15);
    memmove(p->name, last, name_len);
    p->name[name_len] = '\0';

    return argc;

bad:
    if (ip != NULL)
    {
        inode_unlock(ip);
        inode_put(ip);
    }
    if (new_pgtbl != NULL)
    {
        vm_unmappages(new_pgtbl, TRAPFRAME, PGSIZE, true);
        vm_unmappages(new_pgtbl, TRAMPOLINE, PGSIZE, false);
        /* 递归释放页表中管理的所有页面 */
        uvm_destroy_pgtbl(new_pgtbl);
    }
    else if (new_tf != NULL)
    {
        pmem_free((uint64)new_tf, true);
    }
    return -1;
}
