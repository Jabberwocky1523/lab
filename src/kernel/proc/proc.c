#include "mod.h"

// 这个文件通过make build生成, 是proczero对应的ELF文件
#include "../../user/initcode.h"
#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t *old, context_t *new);

// in trap/trap_user.c
extern void trap_user_return();

// 第一个用户进程
static proc_t proczero;

// 获得一个初始化过的用户页表
// 完成trapframe和trampoline的映射
// 参考 xv6: kernel/proc.c proc_pagetable()
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    // 为用户页表分配一个物理页(内核内存)
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    memset(pgtbl, 0, PGSIZE);

    // 映射trampoline页 (仅supervisor访问, 不加PTE_U)
    // 用户<->内核切换的公共代码, 映射到最高虚拟地址
    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 映射trapframe页 (仅supervisor访问, 不加PTE_U)
    // 用于保存/恢复用户寄存器状态
    vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);

    return pgtbl;
}

/*
    第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trapoline   (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (1 page)
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问

    注意: 用用户空间的地址映射需要标记 PTE_U

    参考 xv6: kernel/proc.c userinit() + allocproc()
*/
void proc_make_first()
{
    proc_t *p = &proczero;

    // 1. 分配trapframe页面 (内核内存)
    p->tf = (trapframe_t *)pmem_alloc(true);
    memset(p->tf, 0, PGSIZE);

    // 2. 创建用户页表 (映射trampoline和trapframe)
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);

    // 3. 设置内核栈虚拟地址 (物理页已在kvm_init中分配和映射)
    p->kstack = KSTACK(0);

    // 4. 分配用户栈 (1页, 用户内存, 位于trapframe下方)
    p->ustack_npage = 1;
    uint64 ustack_pa = (uint64)pmem_alloc(false); // 用户物理内存
    uint64 ustack_va = TRAPFRAME - PGSIZE;        // 栈在trapframe紧下方
    vm_mappages(p->pgtbl, ustack_va, ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);

    // 5. 加载initcode到用户空间 USER_BASE处
    uint64 code_pa = (uint64)pmem_alloc(false); // 用户物理内存
    vm_mappages(p->pgtbl, USER_BASE, code_pa, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);
    memmove((void *)code_pa, initcode, initcode_len);
    p->heap_top = USER_BASE + PGSIZE;

    // 6. 设置用户态入口PC = USER_BASE (initcode起始地址)
    p->tf->user_to_kern_epc = USER_BASE;
    // 用户栈初始栈顶
    p->tf->sp = ustack_va + PGSIZE;

    // 7. 设置当前CPU的运行进程为proczero
    mycpu()->proc = p;

    // 8. 切入用户态 (不再返回)
    trap_user_return();
}