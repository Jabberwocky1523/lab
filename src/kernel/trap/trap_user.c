#include "mod.h"
#include "../../user/syscall_num.h"

// in trampoline.S
extern char trampoline[];  // 内核和用户切换的代码
extern char user_vector[]; // 用户触发陷阱进入内核
extern char user_return[]; // 内核处理完毕返回用户

// in trap.S
extern char kernel_vector[]; // 内核态trap处理流程, 进入内核后应当切换中断处理入口

// in trap_kernel.c
extern char *interrupt_info[16]; // 中断错误信息
extern char *exception_info[16]; // 异常错误信息

// in mem/kvm.c
extern pgtbl_t kernel_pgtbl;

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
void trap_user_handler()
{
    // 1. 进入内核态后, 将 trap 入口重写为 kernel_vector
    w_stvec((uint64)kernel_vector);

    proc_t *p = myproc();
    trapframe_t *tf = p->tf;

    uint64 sepc = r_sepc();
    uint64 scause = r_scause();

    // 2. 记录发生 trap 的 PC 到 trapframe, 确保正确返回用户态
    tf->user_to_kern_epc = sepc;

    int trap_id = scause & 0xf;

    /* 高位 bit 标识是中断还是异常 */
    if (scause & 0x8000000000000000ul)
    {
        // 中断处理 (与 trap_kernel_handler 相同)
        switch (trap_id)
        {
        case 1:
            timer_interrupt_handler();
            break;
        case 9:
            external_interrupt_handler();
            break;
        default:
            printf("\nunexpected user interrupt: %s\n", interrupt_info[trap_id]);
            printf("trap_id = %d, sepc = %p\n", trap_id, sepc);
            panic("trap_user_handler: interrupt");
        }
    }
    else
    {
        // 异常处理
        switch (trap_id)
        {
        case 8: // Environment call from U-mode (系统调用)
            switch (tf->a7)
            {
            case SYS_helloworld:
                printf("helloworld\n");
                break;
            default:
                printf("unknown syscall number: %d\n", tf->a7);
                panic("trap_user_handler: unknown syscall");
            }
            // 系统调用完成后 PC 指向下一条指令
            tf->user_to_kern_epc += 4;
            break;
        default:
            printf("\nunexpected user exception: %s\n", exception_info[trap_id]);
            printf("trap_id = %d, sepc = %p, stval = %p\n", trap_id, sepc, r_stval());
            panic("trap_user_handler: exception");
        }
    }

    // 处理完毕, 返回用户态
    trap_user_return();
}

// 调用user_return()
// 内核态返回用户态
void trap_user_return()
{
    proc_t *p = myproc();
    trapframe_t *tf = p->tf;

    // 1. 保存内核态运行信息到 trapframe (供下次 user_vector 陷入时恢复)
    tf->user_to_kern_satp = MAKE_SATP(kernel_pgtbl);
    tf->user_to_kern_sp = p->kstack + PGSIZE;
    tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    tf->user_to_kern_hartid = mycpuid();

    // 2. 将 stvec 设为 user_vector (用户态运行时 trap 走 trampoline)
    w_stvec((uint64)user_vector);

    // 3. 将 sepc 设为用户程序计数器, 确保返回用户态后 PC 正确
    w_sepc(tf->user_to_kern_epc);

    // 4. 设置 sstatus: SPP=U-mode, SUM=1 (允许S-mode访问PTE_U页面如TRAPFRAME),
    //    SPIE=1 (sret后用户态中断使能)
    uint64 sstatus = r_sstatus();
    sstatus &= ~SSTATUS_SPP;
    sstatus |= SSTATUS_SUM | SSTATUS_SPIE;
    w_sstatus(sstatus);

    // 5. 设置 sscratch 指向 trapframe (user_vector 通过 sscratch 找到 trapframe)
    w_sscratch((uint64)TRAPFRAME);

    // 6. 准备参数并调用 trampoline.S 中的 user_return(trapframe, satp)
    //    user_return 切换到用户页表、恢复用户寄存器、sret 进入 U-mode
    void (*fn)(void *, uint64) = (void (*)(void *, uint64))user_return;
    fn((void *)TRAPFRAME, MAKE_SATP(p->pgtbl));
}