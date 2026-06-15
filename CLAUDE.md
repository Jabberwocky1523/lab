# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

这是一个面向教学的 RISC-V 裸机内核实验项目（ECNU-OSLAB-2025-TASK），运行在 QEMU 模拟器上。当前分支 `lab8` 的工作是**文件系统的数据组织与层次结构**——在 lab7 实现的 block-level 磁盘管理基础上，构建 inode 索引节点和 dentry 目录项系统。

## 构建、运行与调试

```bash
# 完整构建 (内核 + 磁盘映像)
make build

# 构建并启动 QEMU 运行内核
make run

# 构建并以 GDB 调试模式启动 QEMU
make debug

# 清理所有构建产物
make clean
```

- 交叉编译工具链前缀: `riscv64-linux-gnu-`，定义在 [common.mk](common.mk)
- 编译器: `riscv64-linux-gnu-gcc`，链接器: `riscv64-linux-gnu-ld`
- `mkfs` (磁盘映像生成工具) 用**宿主 gcc** 编译，不是交叉编译器
- GDB 端口根据 UID 动态计算：`expr (id -u) % 5000 + 25000`

## 整体架构

### 模块组织

每个内核模块（arch、boot、lock、lib、mem、trap、proc、syscall、fs）都遵循统一的三文件约定：

| 文件 | 作用 |
|------|------|
| `type.h` | 数据结构定义和常量宏 |
| `mod.h` | 模块聚合头文件，`#include` 本模块的 `type.h`、`method.h` 及依赖模块的 `mod.h` |
| `method.h` | 本模块的公开 API 声明 |

`src/kernel/main.c` 在所有模块之上，按顺序调用各子系统的初始化。

### 启动流程

[main.c](src/kernel/main.c) `main()` → CPU0 执行: `pmem_init()` → `kvm_init()` → `kvm_inithart()` → `mmap_init()` → `virtio_disk_init()` → `proc_init()` → `proc_make_first()` → `trap_kernel_init()` → `trap_kernel_inithart()` → `proc_scheduler()`. CPU1+ 仅初始化 kvm/trap 然后进入调度器。

### 链接布局

[kernel.ld](kernel.ld) — 内核加载地址 `0x80000000`，128MB RAM。`.text` 段包含 trampoline 页（必须恰好 1 页）。`ALLOC_BEGIN` 到 `ALLOC_END` 表示物理内存可分配范围。

### 文件系统栈（本次实验核心）

```
路径解析 (dentry.c)
    ↓
inode 数据读写 (inode.c)
    ↓
bitmap 分配/释放 (bitmap.c)
    ↓
buffer 缓存层 (buf.c)
    ↓
virtio 磁盘驱动 (virtio.c)
```

磁盘布局 (由 `super_block_t` 定义): super block (block 0) → inode bitmap → inode region → data bitmap → data region.

### inode 索引结构 ([type.h](src/kernel/fs/type.h#L153-L159))

```
直接映射:   index[0..9]   → 10 个数据块 (40KB)
一级间接:   index[10..11]  → 2 × 1024 个数据块 (8MB)
二级间接:   index[12]      → 1 × 1024 × 1024 个数据块 (4GB)
```

`inode_disk_t` 是磁盘上 64 字节的持久化结构。`inode_t` 是内存中的表示，带 ref 计数和睡眠锁。内存最多缓存 64 个活跃 inode。

## 关键实现细节

- **pmem_alloc 是 LIFO**：连续分配返回降序地址。需要连续物理页时，先分配到临时数组再取最低地址。
- **`inode_write_data` 只修改内存中的 `disk_info`**：如果之后其他代码通过 `path_to_inode`（会重新 `inode_get`）读取，必须先调用 `inode_rw(ip, true)` 写回磁盘。
- **buffer 缓存可能有残留数据**：`bitmap_free_block` 释放的块被重新分配后，buffer cache 中可能还保留旧数据。给新分配的目录块写入数据前需要清零。
- **mkfs 用 C23 标准编译**：`src/mkfs/mkfs.h` 使用 `#include <stdbool.h>` 而非自定义 bool 枚举，避免新版 GCC 的 `false`/`true` 关键字冲突。

## 调试

- `.vscode/` 和 `registers.xml` 已配置好可视化调试环境
- `make debug` 启动带 GDB stub 的 QEMU，然后可用 riscv64 gdb 连接动态计算的端口
