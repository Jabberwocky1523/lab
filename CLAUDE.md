# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 在本仓库中工作提供指导。

## 构建与运行

```bash
make build          # 构建内核 ELF + 用户 initcode → target/
make run            # 构建并在 qemu-system-riscv64 中运行
make debug          # 构建并启动 QEMU，同时挂载 GDB stub
make clean          # 删除 target/ 及生成文件
```

交叉编译工具链：`riscv64-linux-gnu-{gcc,ld,objcopy,objdump}`。工具链配置见 [common.mk](common.mk)。

GDB 端口动态计算：`$(id -u) % 5000 + 25000`。执行 `make debug` 会从 `.gdbinit.tmpl-riscv` 自动生成 `.gdbinit` 并填入正确端口。

## 架构概览

这是一个 **RISC-V 64 位教学操作系统内核**（ECNU OSLAB），参考 xv6 设计。运行在 `qemu-system-riscv64` 上，使用 SV39 虚拟内存，支持多核 CPU。

### 启动流程

```
M-mode: entry.S → start.c → mret → S-mode: main()
```

- [src/kernel/boot/start.c](src/kernel/boot/start.c) 在 M-mode 下运行：将 hartid 存入 `tp` 寄存器，通过 `medeleg`/`mideleg` 将所有 trap 委托给 S-mode，初始化 M-mode 时钟，配置 PMP 允许访问全部物理内存，然后 `mret` 到 `main()` 进入 S-mode。
- [src/kernel/main.c](src/kernel/main.c)：CPU0 初始化内存、页表、trap，创建第一个用户进程（`proc_make_first`），然后设置 `started=1`。其他 CPU 自旋等待 `started`，之后初始化各自的页表和 trap 向量。

### 内存模型 (SV39)

物理内存布局（由 [kernel.ld](kernel.ld) 定义）：
- `0x80000000` — 内核 `.text`（代码段）
- trampoline 页通过 `trampsec` section 嵌入在 `.text` 中，对齐到一个页面
- `KERNEL_DATA` — `.rodata`、`.data`、`.bss`
- `ALLOC_BEGIN` 至 `0x80000000 + 128M`（`ALLOC_END`）— 可供分配器使用的空闲内存

虚拟地址空间常量（[src/kernel/mem/type.h](src/kernel/mem/type.h)）：

| 常量 | 地址 | 用途 |
|---|---|---|
| `TRAMPOLINE` | `VA_MAX - PGSIZE` | 内核↔用户切换代码（在内核和用户页表中恒等映射） |
| `TRAPFRAME` | `TRAMPOLINE - PGSIZE` | 寄存器保存/恢复区域（仅存在于用户页表） |
| `KSTACK(n)` | `TRAPFRAME - (n+1)*2*PGSIZE` | 每个进程的内核栈（仅存在于内核页表） |
| `USER_BASE` | `PGSIZE` (0x1000) | 用户代码/数据起始地址；第 0 页不映射作为 guard page |

SV39 使用三级页表：`VA[VPN2:VPN1:VPN0:offset]`（9:9:9:12 位）。核心 VM 函数 `vm_getpte`、`vm_mappages`、`vm_unmappages` 位于 [src/kernel/mem/kvm.c](src/kernel/mem/kvm.c)。

### 物理内存分配器

[src/kernel/mem/pmem.c](src/kernel/mem/pmem.c) — 空闲页组成单链表，利用空闲页的前 8 字节作为 `next` 指针。页面分为内核内存（前 `KERN_PAGES` = 1024 页）和用户内存（剩余部分）。`pmem_alloc(true)` 分配内核内存；`pmem_alloc(false)` 分配用户内存。

### Trap 处理

**两阶段设计**：trampoline 汇编代码处理特权级跨越，C 语言 handler 包含业务逻辑。

- **内核 trap**（[src/kernel/trap/trap_kernel.c](src/kernel/trap/trap_kernel.c)）：`kernel_vector`（在 [trap.S](src/kernel/trap/trap.S) 中）保存上下文，调用 `trap_kernel_handler()`。处理时钟中断（仅 CPU0 更新时钟滴答）和外设/PLIC 中断（UART）。通过 `trap.S` 中的 `sret` 返回。
- **用户 trap**（[src/kernel/trap/trap_user.c](src/kernel/trap/trap_user.c)）：用户态发生 trap 时，硬件跳转到 `stvec` → trampoline 中的 `user_vector`。trampoline 将寄存器保存到 `trapframe`，加载内核栈和内核页表，跳转到 `trap_user_handler()`。处理完毕后，`trap_user_return()` 通过 trampoline 中的 `user_return` 设置返回路径：切换到用户页表、恢复寄存器、`sret` 回到 U-mode。
- **stvec 切换**：在内核中 → `kernel_vector`；返回用户态前 → `TRAMPOLINE + (user_vector - trampoline)`。这确保内核态的 trap 走内核 handler，用户态的 trap 走 trampoline。

### 进程模型

[src/kernel/proc/proc.c](src/kernel/proc/proc.c) — 目前仅实现了第一个进程（`proczero`，一个静态 `proc_t`）。

`proc_pgtbl_init()` 创建用户页表，映射 trampoline 和 trapframe（不加 `PTE_U`，仅 supervisor 可访问）。

`proc_make_first()` 构建第一个进程的地址空间：
1. 分配 trapframe 页面（内核内存）
2. 创建用户页表
3. 设置内核栈为 `KSTACK(0)`（物理页已在 `kvm_init` 中分配并映射）
4. 在 `TRAPFRAME - PGSIZE` 分配 1 页用户栈
5. 在 `USER_BASE` 加载 initcode（代码+数据占 1 页）
6. 设置 `mycpu()->proc = &proczero`
7. 调用 `trap_user_return()` 进入 U-mode — 此函数不再返回

内核线程间上下文切换使用 [src/kernel/proc/swtch.S](src/kernel/proc/swtch.S) 中的 `swtch(old_ctx, new_ctx)`（仅保存/恢复 callee-saved 寄存器）。

### 用户程序

[src/user/initcode.c](src/user/initcode.c) — 第一个（也是目前唯一的）用户程序。两次调用 `syscall(SYS_helloworld)` 后死循环。编译为裸二进制，然后通过 `xxd -i` 转换为 `initcode.h`（C 数组），链接进内核镜像。

系统调用路径：`ecall`（U-mode，scause=8）→ trampoline `user_vector` → `trap_user_handler()` 从 `p->tf->a7` 读取系统调用号，处理后 `user_to_kern_epc += 4` 跳过 ecall 指令返回。

### 模块约定

每个子系统遵循 `method.h` / `mod.h` / `type.h` 模式：
- `type.h` — 结构体和常量定义
- `method.h` — 函数声明
- `mod.h` — 按依赖顺序引入全部头文件（`type.h` → 依赖模块的 `mod.h` → 本模块 C 头文件 → `method.h`）

### 关键 RISC-V CSR 寄存器

- `satp` — 页表根地址 + SV39 模式
- `stvec` — trap 处理函数地址（在 `kernel_vector` 和 trampoline `user_vector` 之间切换）
- `sstatus` — `SPP`（之前的特权级）、`SPIE`/`SIE` 中断使能位
- `sepc` — trap 发生时的 PC；`scause` — trap 原因；`stval` — trap 辅助信息
- `sscratch` — trampoline 保存寄存器时临时存放 `a0`
- `tp` — 存放 hartid（启动时设置，两种特权级均可访问）
