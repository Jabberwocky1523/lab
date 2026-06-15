# LAB-6: 单进程走向多进程——进程调度与生命周期

## 代码组织结构

```
ECNU-OSLAB-2025-TASK
├── LICENSE        开源协议
├── .vscode        配置了可视化调试环境
├── registers.xml  配置了可视化调试环境
├── .gdbinit.tmp-riscv xv6自带的调试配置
├── common.mk      Makefile中一些工具链的定义
├── Makefile       编译运行整个项目
├── kernel.ld      定义了内核程序在链接时的布局
├── pictures       README使用的图片目录 (CHANGE, 日常更新)
├── README.md      实验指导书 (CHANGE, 日常更新)
└── src            源码
    ├── kernel     内核源码
    │   ├── arch   RISC-V相关
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── boot   机器启动
    │   │   ├── entry.S
    │   │   └── start.c
    │   ├── lock   锁机制
    │   │   ├── spinlock.c
    │   │   ├── sleeplock.c (TODO, 实现睡眠锁)
    │   │   ├── method.h (CHANGE)
    │   │   ├── mod.h (CHANGE, 增加头文件)
    │   │   └── type.h (CHANGE)
    │   ├── lib    常用库
    │   │   ├── cpu.c
    │   │   ├── print.c
    │   │   ├── uart.c
    │   │   ├── utils.c
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── mem    内存模块
    │   │   ├── pmem.c
    │   │   ├── kvm.c (TODO, kvm_init从单进程内核栈初始化到多进程内核栈初始化)
    │   │   ├── uvm.c
    │   │   ├── mmap.c
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── trap   陷阱模块
    │   │   ├── plic.c
    │   │   ├── timer.c (TODO, 新增timer_wait函数, 增加时钟中断调度逻辑)
    │   │   ├── trap_kernel.c (TODO, 增加时钟中断调度逻辑)
    │   │   ├── trap_user.c (TODO, 增加时钟中断调度逻辑)
    │   │   ├── trap.S
    │   │   ├── trampoline.S
    │   │   ├── method.h (CHANGE, 增加timer_wait函数声明)
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── proc   进程模块
    │   │   ├── proc.c (TODO, 核心工作)
    │   │   ├── swtch.S
    │   │   ├── method.h (CHANGE)
    │   │   ├── mod.h
    │   │   └── type.h (CHANGE)
    │   ├── syscall 系统调用模块
    │   │   ├── syscall.c (CHANGE, 支持新的系统调用)
    │   │   ├── sysfunc.c (TODO, 实现新的系统调用)
    │   │   ├── method.h (CHANGE)
    │   │   ├── mod.h
    │   │   └── type.h (CHANGE)
    │   └── main.c (CHANGE)
    └── user       用户程序
        ├── initcode.c (CHANGE)
        ├── sys.h
        ├── syscall_arch.h
        └── syscall_num.h (CHANGE)
```

---

## LAB-6 完成清单

### 1. [src/kernel/proc/proc.c](src/kernel/proc/proc.c) — 进程管理核心

- **`proc_init()`** — 初始化每个进程的自旋锁，通过 `KSTACK(i)` 设置内核栈地址，所有状态初始化为 `UNUSED`，初始化 `pid_lk` 并将 `global_pid` 设为 1
- **`proc_alloc()`** — 遍历 `proc_list` 寻找 `UNUSED` 槽位，分配 trapframe + 用户页表，设置 `ctx.ra = proc_return`，返回时持有进程锁
- **`proc_free()`** — 销毁用户页表（内部释放 trapframe 物理页），释放 mmap 链表，重置所有字段，状态置为 `UNUSED`
- **`proc_make_first()`** — 通过 `proc_alloc()` 获取 proczero，建立用户栈 + 加载 initcode，解锁后交由调度器接管
- **`proc_return()`** — 释放进程锁并调用 `trap_user_return()` 进入用户态（作为新进程首次被调度时的入口点）
- **`proc_fork()`** — 通过 `proc_alloc()` 分配子进程，复制页表、mmap 链表、trapframe，设置子进程 `a0 = 0`，记录父子关系
- **`proc_yield()`** — 获取进程锁，将状态从 `RUNNING` 改为 `RUNNABLE`，调用 `proc_sched()`，释放锁
- **`proc_reparent()`** — 将退出进程的所有子进程过继给 proczero（参照 xv6 的无锁父→子检查模式）
- **`proc_try_wakeup()`** — 唤醒在 `proc_wait()` 中睡眠的父进程（调用者须持有 parent->lk，参照 xv6 `wakeup1`）
- **`proc_exit()`** — 过继子进程，按父→子顺序获取锁，设为 `ZOMBIE`，唤醒父进程，调用 `proc_sched()` 永不返回
- **`proc_wait()`** — 扫描 `proc_list` 寻找 `ZOMBIE` 子进程，将 `exit_code` 拷贝到用户空间，调用 `proc_free()` 回收；无子进程返回 -1；否则以自身为资源进入 `proc_sleep`
- **`proc_sleep()`** — 原子性地释放外部锁，设为 `SLEEPING`，调用 `proc_sched()`，唤醒后重新获取外部锁
- **`proc_wakeup()`** — 遍历所有进程，唤醒在指定 `sleep_space` 上睡眠的进程
- **`proc_sched()`** — 校验锁/noff/状态/中断状态，调用 `swtch()` 切换到调度器上下文
- **`proc_scheduler()`** — 死循环扫描 `RUNNABLE` 进程，`swtch()` 切入运行，无进程时 `wfi` 等待中断

### 2. [src/kernel/lock/sleeplock.c](src/kernel/lock/sleeplock.c) — 睡眠锁

- `sleeplock_init()`, `sleeplock_holding()`, `sleeplock_acquire()`, `sleeplock_release()` — 基于自旋锁 + `proc_sleep()`/`proc_wakeup()` 实现睡眠锁

### 3. [src/kernel/mem/kvm.c](src/kernel/mem/kvm.c) — 多进程内核栈

- `kvm_init` 中将原来只分配一个内核栈 (`KSTACK(0)`) 改为循环为所有 `N_PROC` 个进程分配内核栈

### 4. [src/kernel/trap/timer.c](src/kernel/trap/timer.c) — 时钟与睡眠

- **`timer_update()`** — 新增 `proc_wakeup(&sys_timer)` 调用，每次时钟滴答后唤醒等待系统时钟的进程
- **`timer_wait()`** — 获取 `sys_timer.lk`，循环调用 `proc_sleep(&sys_timer, &sys_timer.lk)` 直到达到目标 tick 数

### 5. [src/kernel/trap/trap_kernel.c](src/kernel/trap/trap_kernel.c) — 抢占式调度（内核态）

- 在时钟中断处理 (`case 1`) 后，若当前有进程正在运行则调用 `proc_yield()` 放弃 CPU
- 在函数末尾恢复 `sepc` 和 `sstatus` 寄存器（参照 xv6，防止 yield 上下文切换破坏这些寄存器）

### 6. [src/kernel/trap/trap_user.c](src/kernel/trap/trap_user.c) — 抢占式调度（用户态）

- 将 `trap_id` 声明提升到 `if/else` 外部作用域
- 在 `trap_user_return()` 返回用户态之前，若为时钟中断则调用 `proc_yield()`

### 7. [src/kernel/syscall/sysfunc.c](src/kernel/syscall/sysfunc.c) — 新增系统调用

- `sys_print_str()`, `sys_print_int()`, `sys_getpid()`, `sys_fork()`, `sys_wait()`, `sys_exit()`, `sys_sleep()` — 全部实现

### 8. 头文件更新

- [src/kernel/proc/type.h](src/kernel/proc/type.h)：添加 `#include "../lock/type.h"`（引入 `spinlock_t` 类型）
- [src/kernel/lock/mod.h](src/kernel/lock/mod.h)：添加 `#include "../proc/mod.h"`（sleeplock 依赖 proc 的睡眠/唤醒函数）
- [src/kernel/trap/mod.h](src/kernel/trap/mod.h)：添加 `#include "../proc/mod.h"`（trap 处理函数需要调用 `proc_yield`）