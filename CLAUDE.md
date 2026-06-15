# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

这是 **ECNU-OSLAB-2025-TASK**，一个基于 xv6-riscv 的教学操作系统，运行在 RISC-V 64 位架构上（QEMU `virt` 机器）。当前分支 `lab7` 主要实现文件系统的磁盘管理部分，包括 VirtIO 块设备驱动、缓冲区缓存、bitmap 位图管理。

## 构建与运行

```bash
# 构建整个项目（内核 + 用户程序 + 磁盘映像）
make build

# 构建并启动 QEMU 运行
make run

# 构建并以 GDB 调试模式启动 QEMU（QEMU 会暂停等待 GDB 连接）
make debug

# 清理构建产物
make clean
```

- **工具链**: `riscv64-linux-gnu-` 交叉编译器
- **QEMU**: `qemu-system-riscv64`，模拟 2 核 CPU、128MB 内存、virtio-blk 磁盘
- **GDB 端口**：动态计算为 `id -u % 5000 + 25000`（如 uid=1000 则为 26000）

## 核心架构

### 启动流程（三阶段）

1. **M-mode 汇编** (`boot/entry.S`): QEMU 跳转到物理地址 `0x80000000`，设置 per-CPU 内核栈，跳转到 `start()`
2. **M-mode C** (`boot/start.c`): 禁用分页，将所有异常/中断委托给 S-mode，初始化 CLINT 定时器，配置 PMP，执行 `mret` 降级到 S-mode 进入 `main()`
3. **S-mode 内核** (`main.c`): 仅 hart 0 初始化共享资源（内存、磁盘、进程），其他 hart 自旋等待 `started` 标志后初始化自己的页表和中断向量，然后全部进入调度器

### 模块依赖层次（自底向上）

```
arch/  →  lib/  →  lock/  →  mem/  →  trap/（最宽依赖，关联所有模块）
                                    →  proc/（与 trap 同级）
                                    →  fs/（与 proc 同级）
```

每个子目录遵循 `type.h`（类型定义）→ `method.h`（函数声明）→ `mod.h`（汇总 include 依赖）的约定。

### 虚拟内存布局（SV39，39 位地址空间）

| 虚拟地址 | 用途 |
|---------|------|
| `0x80000000` | KERNEL_BASE（内核物理地址恒等映射） |
| `ALLOC_BEGIN` ~ `ALLOC_END`（128MB） | 可分配的物理内存 |
| `0x1000` (USER_BASE) | 用户程序起始地址（第0页为 guard page） |
| `MMAP_BEGIN` ~ `MMAP_END` | 进程 mmap 区域（每进程 ~64MB） |
| `KSTACK(pid)` | 每进程内核栈 |
| `TRAPFRAME` | 用户↔内核 trapframe（1 页） |
| `TRAMPOLINE`（VA_MAX - PGSIZE） | 用户/内核共享的上下文切换代码（1 页） |

### 关键子系统

- **mem/kvm.c**: 内核页表，恒等映射 UART、CLINT、PLIC、VIRTIO MMIO、全部物理内存、trampoline。`kernel_pgtbl` 设为非 static 以允许 virtio 驱动通过 `vm_getpte` 做 DMA 地址翻译
- **mem/uvm.c**: 用户虚拟内存管理，包括 COW fork、按需用户栈增长
- **proc/proc.c**: 进程管理，`proc_list[32]` 静态数组，轮转调度，`proc_sleep`/`proc_wakeup` 基于任意 `void*` 地址
- **trap/**: trampoline 机制实现用户↔内核切换；plic.c 处理外部中断（UART + virtio 磁盘）；timer.c 处理时钟中断
- **lock/**: `spinlock`（带 `push_off`/`pop_off` 中断嵌套保护）+ `sleeplock`（用于缓冲区等长时间持有的资源）
- **fs/**: 文件系统模块 — `buffer.c`（LRU 双链表缓冲区）、`bitmap.c`（基于缓冲区位图分配器）、`virtio.c`（VirtIO-MMIO 块设备驱动）、`fs.c`（超级块读取和文件系统初始化）

### 关键设计决策

1. **`vm_getpte` 处理 NULL 页表**：当 `pgtbl == NULL` 时自动使用 `kernel_pgtbl`，允许 virtio 驱动将内核栈虚拟地址翻译为物理地址用于 DMA 描述符
2. **`fs_init` 调用时机**：在 `proc_return()` 中首次调度时调用（而非 `main()`），因为磁盘 I/O 需要进程上下文支持 sleep/wakeup
3. **LRU 缓冲区管理**：双链表设计 — 活跃链表（ref > 0）和非活跃链表（ref == 0）。命中时移到活跃链表头部；未命中时淘汰非活跃链表尾部
4. **用户程序嵌入**：`src/user/initcode.c` 编译后通过 `xxd -i` 转为 C 字节数组嵌入内核，作为第一个用户进程（init 进程）

### 系统调用

系统调用跳转表在 `syscall/syscall.c` 中，通过 `a7` 寄存器索引。参数从 trapframe 的 `a0`-`a5` 读取。当前共 21 个系统调用，涵盖进程管理（fork/wait/exit/sleep）、内存管理（brk/mmap/munmap）、文件系统操作（alloc_block/free_block/alloc_inode/free_inode/read_block/write_block 等）和调试接口（show_bitmap/show_buffer/flush_buffer 等）。

### 调试

VSCode 已配置可视化调试（`.vscode/launch.json`），使用 `gdb-multiarch` 配合 `registers.xml` 显示 RISC-V 寄存器。调试步骤：

1. 终端运行 `make debug`（启动 QEMU 并暂停等待 GDB）
2. 在 VSCode 中按 F5 启动调试会话，连接到 GDB 端口

### 磁盘映像

```bash
# 每次 make 构建时自动重新生成 disk.img
# 手动构建：
gcc -Werror -Wall -I. -o target/mkfs/mkfs src/mkfs/mkfs.c
target/mkfs/mkfs target/mkfs/disk.img
```

磁盘映像由 `src/mkfs/mkfs.c` 生成，包含超级块（魔数 `0x12341234`）、inode 位图区、数据块位图区、inode 区和数据块区。
