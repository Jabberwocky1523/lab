# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

ECNU-OSLAB-2025-TASK 教学操作系统，基于 RISC-V (rv64) 架构，运行在 QEMU 虚拟机上。当前分支 `lab6` 主题为"单进程走向多进程——进程调度与生命周期"。

## 构建与运行

```bash
# 构建整个项目（内核 + 用户程序）
make build

# 构建并启动 QEMU 运行
make run

# 构建并以调试模式启动 QEMU（挂起等待 GDB 连接）
make debug

# 清理构建产物
make clean
```

- 工具链前缀：`riscv64-linux-gnu-`（定义在 [common.mk](common.mk)）
- QEMU 配置：`qemu-system-riscv64`，2 个 CPU 核心，128M 内存，无图形界面
- GDB 端口由 `$(id -u) % 5000 + 25000` 动态计算
- 用户程序 [initcode.c](src/user/initcode.c) 被编译为二进制后通过 `xxd -i` 转换为 C 头文件 [initcode.h](src/user/initcode.h)，嵌入内核

## 代码架构

### 模块依赖层次（自上而下）

```
arch  →  lib  →  lock  →  proc  →  syscall  →  trap
                                   ↘  mem     ↗
```

- **arch**：RISC-V 特权架构相关定义（CSR 寄存器读写宏、sstatus/satp/stvec 等位操作）
- **lib**：基础库（UART 输出、printf、字符串/内存工具、CPU 信息）
- **lock**：自旋锁 (`spinlock_t`) 和睡眠锁 (`sleeplock_t`)，睡眠锁依赖 proc 模块的睡眠/唤醒
- **mem**：物理内存分配 (`pmem`)、内核页表 (`kvm`，映射内核代码+数据+内核栈+ trampoline)、用户态页表和 mmap (`uvm`, `mmap`)
- **proc**：进程管理核心——进程结构体、分配/释放/调度/睡眠/唤醒/fork/exit/wait
- **syscall**：系统调用跳转表 + 参数解析 + 各 sys_* 实现函数
- **trap**：中断/异常处理（内核态 trap、用户态 trap、时钟中断、PLIC 外设中断、trampoline 切换）
- **user**：用户态 initcode，通过 `syscall(num, args...)` 宏发起系统调用

### 启动流程

1. `_entry`（[boot/entry.S](src/kernel/boot/entry.S)）→ `start()`（[boot/start.c](src/kernel/boot/start.c)）：设置 M-mode 中断委托，初始化栈，切换到 S-mode 跳转至 `main()`
2. CPU0 的 `main()` 依次初始化：`print_init` → `pmem_init` → `kvm_init` → `kvm_inithart` → `mmap_init` → `proc_init` → `proc_make_first` → `trap_kernel_init` → `trap_kernel_inithart`
3. CPU1 等待 CPU0 初始化完成后仅执行 `kvm_inithart` + `trap_kernel_inithart`
4. 所有 CPU 最终进入 `proc_scheduler()` 死循环，选择 RUNNABLE 进程执行

### 进程状态机

```
UNUSED → RUNNABLE → RUNNING → RUNNABLE  (抢占/让出CPU)
                            → SLEEPING → RUNNABLE  (睡眠/唤醒)
                            → ZOMBIE → UNUSED      (退出/回收)
```

状态定义在 [src/kernel/proc/type.h](src/kernel/proc/type.h) 的 `enum proc_state`。

### 用户态 ↔ 内核态切换机制

- **用户→内核**：硬件触发 trap → `user_vector`（trampoline.S）→ `trap_user_handler`（trap_user.c）→ 根据 scause 分发（系统调用/中断/缺页异常）
- **内核→用户**：`trap_user_return`（trap_user.c）→ `user_return`（trampoline.S）→ 切换页表、恢复寄存器、sret
- **内核态 trap**：由 `kernel_vector`（trap.S）→ `trap_kernel_handler`（trap_kernel.c）处理
- Trampoline 页同时映射在内核和用户页表中（地址 `TRAMPOLINE = VA_MAX - PGSIZE`），使切换过程中 PC 始终有效
- Trapframe 页紧随 trampoline 下方（`TRAPFRAME`），保存/恢复全部通用寄存器

### 关键数据结构

- **`proc_t`**（[proc/type.h](src/kernel/proc/type.h)）：包含 pid、锁、状态、父子指针、exit_code、sleep_space、用户页表、trapframe、内核栈地址、上下文（ra+sp+callee-saved 寄存器）。最多 32 个进程
- **`trapframe_t`**：位于 TRAPFRAME 页面，保存用户→内核切换时的完整 CPU 状态
- **`context_t`**：仅包含 ra、sp 和 callee-saved 寄存器，用于 `swtch()` 在同优先级上下文间切换
- **物理内存**：通过空闲页链表管理（[mem/type.h](src/kernel/mem/type.h)），`ALLOC_BEGIN ~ ALLOC_END` 为可分配区域，前 `KERN_PAGES` 页归内核

### 模块文件约定

每个子模块遵循统一结构：
- `type.h`：数据结构定义（`#pragma once` 守卫）
- `method.h`：函数声明
- `mod.h`：聚合 `type.h` + `method.h` + 依赖模块的 `mod.h`，其他模块只需包含目标模块的 `mod.h` 即可引入所有依赖链

### 系统调用

系统调用号定义在 [src/user/syscall_num.h](src/user/syscall_num.h)（用户态和内核态共用）。当前支持：`brk`、`mmap`、`munmap`、`print_str`、`print_int`、`getpid`、`fork`、`wait`、`exit`、`sleep`。用户态通过 `syscall(num, args...)` 宏（[sys.h](src/user/sys.h)）发起 ecall，内核端由 `syscall()` 根据 `a7` 寄存器值查跳转表分发。

### 抢占式调度

时钟中断到达后，内核态和用户态 trap handler 在处理完时钟后调用 `proc_yield()` 放弃 CPU，实现协作式的时间片轮转。`timer_wait()` 使用 `sys_timer` 睡眠锁 + `proc_sleep/proc_wakeup` 实现进程的定时睡眠。
