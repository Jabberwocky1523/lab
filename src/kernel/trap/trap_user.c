#include "mod.h"

// in trampoline.S
extern char trampoline[];  // 内核和用户切换的代码
extern char user_vector[]; // 用户触发陷阱进入内核
extern char user_return[]; // 内核处理完毕返回用户

// in trap.S
extern char kernel_vector[]; // 内核态trap处理流程, 进入内核后应当切换中断处理入口

// in trap_kernel.c
extern char *interrupt_info[16]; // 中断错误信息
extern char *exception_info[16]; // 异常错误信息

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
// 参考 xv6: kernel/trap.c usertrap()
void trap_user_handler()
{
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    // 确认trap来自U-mode
    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: not from user mode");

    proc_t *p = myproc();

    // 保存用户PC到trapframe
    p->tf->user_to_kern_epc = sepc;

    // 切换到内核态trap处理向量 (在内核中发生的trap应由kernel_vector处理)
    w_stvec((uint64)kernel_vector);

    int trap_id = scause & 0xf;

    if (scause & 0x8000000000000000ul)
    {
        // 1. 中断处理
        switch (trap_id)
        {
        case 1: // S-mode software interrupt (来自M-mode的时钟中断)
            timer_interrupt_handler();
            break;
        case 5: // S-mode timer interrupt
            timer_interrupt_handler();
            break;
        case 9: // S-mode external interrupt (PLIC, 如UART)
            external_interrupt_handler();
            break;
        default:
            printf("\nunexpected user interrupt: %s\n", interrupt_info[trap_id]);
            printf("trap_id = %d, sepc = %p\n", trap_id, sepc);
            panic("trap_user_handler: unexpected interrupt");
        }
    }
    else
    {
        // 2. 异常处理
        switch (trap_id)
        {
        case 8: // Environment call from U-mode (系统调用)
        {
            // sepc指向ecall指令, 返回时应跳到下一条指令
            p->tf->user_to_kern_epc += 4;

            // 调用系统调用分发器
            syscall();
            break;
        }
        case 12: // Instruction page fault
        case 13: // Load page fault
        case 15: // Store/AMO page fault
        {
            uint64 fault_addr = r_stval();

            printf("\npage fault in user mode: %s\n", exception_info[trap_id]);
            printf("  sepc = %p, stval = %p\n", sepc, fault_addr);
            printf("  ustack_npage = %d, cur_stack_bottom = %p\n",
                   (int)p->ustack_npage, (void *)(TRAPFRAME - p->ustack_npage * PGSIZE));

            // 尝试栈自动扩展 (仅对 load/store page fault)
            if (trap_id == 13 || trap_id == 15)
            {
                uint64 new_npage = uvm_ustack_grow(p->pgtbl, p->ustack_npage, fault_addr);
                if (new_npage != (uint64)-1)
                {
                    printf("  ustack grown: %d -> %d pages\n",
                           (int)p->ustack_npage, (int)new_npage);
                    p->ustack_npage = new_npage;
                    // 栈扩展成功, 返回用户态重试该指令
                    break;
                }
                printf("  ustack grow failed\n");
            }

            panic("trap_user_handler: page fault");
            break;
        }
        default:
            printf("\nunexpected user exception: %s\n", exception_info[trap_id]);
            printf("  trap_id = %d, sepc = %p, stval = %p\n", trap_id, sepc, r_stval());
            panic("trap_user_handler: unexpected exception");
        }
    }

    // 如果是时钟中断, 当前进程放弃CPU使用权
    if ((scause & 0x8000000000000000ul) && (trap_id == 1 || trap_id == 5))
        proc_yield();

    // 处理完毕, 返回用户态
    trap_user_return();
}

// 调用user_return()
// 内核态返回用户态
// 参考 xv6: kernel/trap.c usertrapret()
void trap_user_return()
{
    proc_t *p = myproc();

    // 关闭中断, 准备切换回用户态
    intr_off();

    // 设置stvec指向user_vector (位于trampoline页内)
    // 当下次从用户态trap时, 硬件会跳转到此处
    uint64 fn = TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline);
    w_stvec(fn);

    // 填充trapframe中内核侧字段, 供下次user_vector进入内核时使用
    p->tf->user_to_kern_satp = r_satp();                        // 内核页表
    p->tf->user_to_kern_sp = p->kstack + PGSIZE;                // 内核栈顶
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler; // 内核trap处理函数
    p->tf->user_to_kern_hartid = r_tp();                        // hartid

    // 设置sstatus: 返回用户模式
    uint64 status = r_sstatus();
    status &= ~SSTATUS_SPP; // 清除SPP -> User mode
    status |= SSTATUS_SPIE; // 使能用户态中断
    w_sstatus(status);

    // 设置用户程序计数器 (ecall的下一条, 或进程入口地址)
    w_sepc(p->tf->user_to_kern_epc);

    // 构造用户页表的satp值
    uint64 satp = MAKE_SATP(p->pgtbl);

    // 跳转到trampoline中的user_return
    // 它负责切换页表、恢复用户寄存器、sret到用户态
    fn = TRAMPOLINE + ((uint64)user_return - (uint64)trampoline);
    ((void (*)(uint64, uint64))fn)(TRAPFRAME, satp);
}