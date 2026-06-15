# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

这是 ECNU-OSLAB-2025-TASK 的 Lab9：一个基于 RISC-V 64 位架构的教学操作系统（类 xv6）。目前处于实验最后阶段，实现了物理内存管理、虚拟内存、进程管理、文件系统（virtio 磁盘驱动 → 缓冲层 → bitmap 分配器 → inode 层 → 目录/路径层 → 文件层 → 设备抽象层），以及 22 个系统调用。

工具链目标架构：`riscv64-linux-gnu-`，使用 `qemu-system-riscv64` 模拟器。

## 常用命令

```bash
make build          # 编译内核 + 用户程序 + mkfs + 磁盘映像
make run            # build 后启动 QEMU（2 核，128M 内存，无图形界面）
make debug          # build 后启动 QEMU 并暂停等待 GDB 连接（端口由 UID 计算）
make clean          # 清理 target/、initcode.h、.gdbinit
```

编译产物路径：
- 内核 ELF：`target/kernel/kernel-qemu.elf`
- 磁盘映像：`target/mkfs/disk.img`
- 用户程序 ELF：`target/user/test_*.elf`
- 用户程序被 mkfs 写入磁盘映像，initcode 通过 `xxd -i` 以二进制 blob 嵌入 `src/user/initcode.h`

## 代码架构

### 三文件模式

内核每个模块目录严格遵循 `type.h`（类型定义）+ `method.h`（函数声明）+ `mod.h`（聚合头文件）模式。消费者只需 `#include "某模块/mod.h"` 即可引入该模块的所有类型、函数及其传递依赖。

`mod.h` 的依赖形成了 DAG：`arch`（叶子，无依赖）→ `lib` → `lock` → `mem` → `fs`/`proc`/`syscall` → `trap`（根，依赖一切）。

### 模块职责

| 目录 | 职责 |
|------|------|
| `arch/` | RISC-V 基础类型（uint64/uint32/reg）和 CSR 位掩码常量 |
| `boot/` | M 模式启动：`entry.S` 设置栈后调用 `start.c`，后者配置硬件后通过 mret 降级到 S 模式的 `main()` |
| `lock/` | 自旋锁 (`spinlock_t`) 和睡眠锁 (`sleeplock_t`)。后者内部包装自旋锁，用于可能阻塞的临界区 |
| `lib/` | UART 驱动、console 行缓冲 I/O、`printf`/`panic`、字符串工具、CPU 状态 |
| `mem/` | 物理内存分配器（伙伴空闲链表）、内核 SV39 页表（恒等映射）、用户虚拟内存（copyin/copyout/mmap/堆栈增长）、mmap 区域节点池 |
| `trap/` | PLIC 中断控制器、M 模式定时器、内核/用户 trap 处理、trampoline（共享页切换） |
| `proc/` | 进程管理（32 槽位轮转调度）、`fork`/`wait`/`exit`、ELF 执行（`proc_exec`） |
| `syscall/` | 系统调用分发表（`syscall.c`）+ 14 个 `sys_*` 实现（`sysfunc.c`）+ 参数提取工具 |
| `fs/` | 文件系统六层：virtio 磁盘驱动 → buffer cache → bitmap 分配器 → inode 层 → dentry/路径 → 文件层/设备抽象 |

### 启动流程

```
entry.S (M-mode, 0x80000000) → start.c:start()
  → 配置 mstatus/deleg/timer/PMP
  → mret 跳转到 main.c:main()
    → CPU0: print_init → pmem_init → kvm_init → kvm_inithart(启用分页)
    → mmap_init → virtio_disk_init → proc_init → proc_make_first
    → trap_kernel_init + trap_kernel_inithart
    → CPU1: kvm_inithart + trap_kernel_inithart
    → 两个 CPU 进入 proc_scheduler() 调度循环
```

### 文件系统分层架构

1. **Virtio 磁盘驱动** (`virtio.c`)：MMIO virtio 块设备 `0x10001000`，每次读写一个 4096 字节块
2. **Buffer Cache** (`buf.c`)：16384 个节点，双向循环链表（active + inactive），`buffer_get()` 先查 active 再淘汰 inactive 尾部
3. **Bitmap 分配器** (`bitmap.c`)：管理 on-disk inode_bitmap 和 data_bitmap，通过 buffer cache 读写
4. **Inode 层** (`inode.c`)：64 个内存 inode 缓存，10 个直接块 + 2 个一级间接 + 1 个二级间接（最大文件 ~4GB）。`inode_put` 在 ref==1 且 nlink==0 时自动释放所有资源
5. **目录/路径层** (`dentry.c`)：目录 inode 中存储 `dentry_t[~64]`（60 字符名 + 4 字节 inode_num）。路径解析支持绝对路径和相对路径（从 `cwd` 出发），`.` 和 `..` 语义
6. **文件层** (`fs.c`)：128 入口全局 `file_table`，`file_read/write` 按 inode 类型（DATA/DIR/DEVICE）分发
7. **设备抽象** (`device.c`)：6 个设备（stdin/stdout/stderr/zero/null/gpt0），各有 major 号和读写函数指针

### 系统调用流程

```
用户程序: sys_open(...) → ecall (a7=syscall_num)
  → user_vector (trampoline.S, 保存 32 个 GPR 到 trapframe)
    → trap_user_handler() → syscall()
      → 读 p->tf->a7 作为系统调用号，查 syscalls[] 跳转表
      → arg_uint32/arg_str 从 p->tf->a0-a5 提取参数
      → 调用 sys_XXX()，结果写入 p->tf->a0
    → trap_user_return() → user_return (恢复 GPR, sret)
```

系统调用号 1-22：brk, mmap, munmap, fork, wait, exit, sleep, getpid, exec, open, close, read, write, lseek, dup, fstat, get_dentries, mkdir, chdir, print_cwd, link, unlink。

### 关键结构体

- **`proc_t`**：pid, name, state, parent, pgtbl, heap_top, ustack_npage, mmap 链表, trapframe, kstack, ctx (callee-saved), cwd (inode_t*), open_file[10]。最多 32 个进程
- **`file_t`**：ip (inode_t*), readable, writable, offset, ref
- **`inode_t`**：inode_num, ref, sleeplock, disk_info (type/nlink/size/index[13])
- **`buffer_t`**：block_num, ref, sleeplock, data[4096], disk 标志位

### 虚拟地址空间布局

```
VA_MAX → TRAMPOLINE (1 页, 共享)
       → TRAPFRAME  (1 页, 每进程)
       → KSTACK(i)  (2 页 × 32 进程)
       → MMAP 区域  (最大 64MB)
       → 用户堆     (USER_BASE + 堆大小, 向上增长)
       → 用户栈     (向下增长)
       → USER_BASE = 0x1000 (0 页作为 guard page)
```

### 编码约定

- 内核代码以 `-std=gnu11` 编译，`-Wall -Werror`，禁用栈保护、PIE，使用 `-mcmodel=medany`
- 内核没有标准库，`printf`/`memmove`/`memset` 等均为自实现
- 睡眠锁用于可能长时间持锁的操作（如磁盘 I/O），自旋锁用于短临界区
- `inode_lock/unlock` 在首次加锁时自动从磁盘读取 `disk_info`（惰性加载）
- 从用户空间读取数据必须通过 `uvm_copyin`/`uvm_copyin_str`，写入用户空间必须通过 `uvm_copyout`，不可直接 `memmove`
- 用户态程序代码在 `src/user/`，编译时除 initcode 外均以 `-Os` 优化体积，链接脚本为 `src/loader/user.ld`
- 系统调用封装通过预处理器元编程实现：`syscall(SYS_xxx, args...)` → `__syscallN(SYS_xxx, args...)` → 内联汇编 `ecall`
- 磁盘映像通过 `src/mkfs/mkfs.c` 生成，该工具以 gcc（而非交叉编译器）编译并在主机运行
