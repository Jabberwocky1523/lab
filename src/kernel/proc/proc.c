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

// in mem/kvm.c
extern pgtbl_t kernel_pgtbl;

// 第一个用户进程
static proc_t proczero;

// 获得一个初始化过的用户页表
// 完成trapframe和trampoline的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    // 分配一个物理页作为用户态根页表
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    memset(pgtbl, 0, PGSIZE);

    // 将trampoline映射到TRAMPOLINE地址(与内核页表映射到同一物理页)
    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    // 同时映射到内核地址: user_return 中切换 satp 后 PC 仍在内核地址,
    // 用户页表必须能在同一 VA 取到 trampoline 指令, 否则 exec_page_fault
    vm_mappages(pgtbl, (uint64)trampoline, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 将trapframe物理页映射到TRAPFRAME地址, 用户态可读写
    vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W | PTE_U);

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
*/
void proc_make_first()
{
    // 1. 设置 pid
    proczero.pid = 0;

    // 2. 为 trapframe 分配物理页 (内核区域)
    trapframe_t *tf = (trapframe_t *)pmem_alloc(true);
    memset(tf, 0, PGSIZE);

    // 3. 创建用户页表 (内部完成 trampoline + trapframe 映射)
    pgtbl_t pgtbl = proc_pgtbl_init((uint64)tf);

    // 4. 分配 ustack 物理页并映射 (紧贴 TRAPFRAME 下方)
    uint64 ustack_va = TRAPFRAME - PGSIZE;
    void *ustack_pa = pmem_alloc(true);
    vm_mappages(pgtbl, ustack_va, (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    proczero.ustack_npage = 1;
    proczero.heap_top = USER_BASE + PGSIZE; // code+data 之后

    // 5. 为 code+data 分配物理页, 拷贝 initcode, 完成映射
    void *code_pa = pmem_alloc(true);
    memmove(code_pa, initcode, initcode_len);
    vm_mappages(pgtbl, USER_BASE, (uint64)code_pa, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);

    // 6. 设置 trapframe: 首次返回用户态时的 PC 和栈指针
    tf->user_to_kern_satp = MAKE_SATP(kernel_pgtbl);
    tf->user_to_kern_sp = KSTACK(0) + PGSIZE;
    tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    tf->user_to_kern_epc = USER_BASE; // 用户程序入口 (initcode)
    tf->user_to_kern_hartid = 0;
    tf->sp = ustack_va + PGSIZE; // 用户栈顶 (栈向下增长)

    // 7. 设置内核上下文: 下次 swtch 回来时从这里开始执行
    proczero.tf = tf;
    proczero.pgtbl = pgtbl;
    proczero.kstack = KSTACK(0);
    proczero.ctx.ra = (uint64)trap_user_return; // swtch 后跳转到 trap_user_return
    proczero.ctx.sp = KSTACK(0) + PGSIZE;       // 内核栈顶

    // 8. 绑定到当前 CPU 并通过 swtch 完成上下文切换
    mycpu()->proc = &proczero;
    swtch(&mycpu()->ctx, &proczero.ctx);
}