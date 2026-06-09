# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

ECNU-OSLAB RISC-V 操作系统内核（基于 xv6 风格），当前处于 **lab-4**（第一个用户进程的诞生）。运行在 QEMU `virt` 机器上，128MB 内存，2 个 CPU 核心。

## 构建与运行

```bash
make build    # 编译内核和用户 initcode（生成 target/ 目录和 src/user/initcode.h）
make run      # 编译并在 QEMU 中运行（qemu-system-riscv64, 无图形界面）
make debug    # 编译并启动 QEMU（暂停等待 GDB 连接），然后用 gdb-multiarch 连接
make clean    # 清理 target/、.gdbinit、src/user/initcode.h
```

调试连接：GDB 端口为 `(uid % 5000) + 25000`，VS Code `launch.json` 已配置好 `gdb-multiarch`。

**工具链依赖**：`riscv64-linux-gnu-gcc`、`riscv64-linux-gnu-ld`、`riscv64-linux-gnu-objcopy`、`qemu-system-riscv64`、`gdb-multiarch`、`xxd`。

## 代码架构

### 三头文件模块约定

每个内核模块严格遵循三层头文件结构，**源文件只 include `mod.h`**：

1. **`type.h`** — 类型定义、结构体、常量、宏（最低层，仅依赖 `arch/type.h`）
2. **`method.h`** — 函数声明（仅 include 自身的 `type.h`）
3. **`mod.h`** — 伞形头文件，include `type.h` + `method.h` + 其他模块依赖

模块位于 `src/kernel/` 下：`arch/`、`boot/`、`lib/`、`lock/`、`mem/`、`trap/`、`proc/`。

### 命名规范

- 类型：`_t` 后缀（`proc_t`、`spinlock_t`、`pgtbl_t`）
- 函数：`模块_函数名`（`pmem_alloc`、`vm_mappages`、`spinlock_acquire`）
- 寄存器操作：`r_`/`w_` 前缀（`r_mhartid()`、`w_satp()`、`r_tp()`）
- 常量：`UPPER_SNAKE_CASE`（`PGSIZE`、`TRAMPOLINE`、`KSTACK`、`NCPU`）

### 启动流程

1. `boot/entry.S`（M-mode）→ 设置栈 → `start()`
2. `boot/start.c`：关分页、保存 hartid 到 `tp`、委托中断到 S-mode（`medeleg`/`mideleg`）、初始化 M-mode 定时器、配置 PMP、`mret` 进入 S-mode
3. `main.c`：CPU 0 执行完整初始化序列（print → pmem → kvm → trap → proc），其他 CPU 自旋等待后调用 `kvm_inithart` + `trap_kernel_inithart`

### 关键内存布局（见 `mem/type.h` + `kernel.ld`）

- 内核加载地址：`0x80000000`
- `TRAMPOLINE = VA_MAX - PGSIZE`：U/S 模式切换的共享代码页（内核和用户页表中恒等映射）
- `TRAPFRAME = TRAMPOLINE - PGSIZE`：每进程陷阱帧
- `KSTACK(procid) = TRAPFRAME - ((procid)+1) * 2 * PGSIZE`：每进程内核栈
- `USER_BASE = PGSIZE`：用户地址空间从第 1 页开始（第 0 页不映射，捕获空指针）
- SV39 三级页表，4KB 页大小

### 自旋锁模式

使用 `push_off()`/`pop_off()` 嵌套中断禁用：`cpu_t.noff` 跟踪每个 CPU 的禁用深度。`spinlock_acquire` 先 `push_off()` 再用 `__sync_lock_test_and_set` 原子获取；`spinlock_release` 先 `__sync_lock_release` 再 `pop_off()`。

### 陷阱/中断处理

- **M-mode**：仅定时器中断。`trap.S` 中的 `timer_vector` 更新 `mtimecmp`，通过 `sip` 触发 S-mode 软件中断。
- **S-mode（内核）**：`trap.S` 中的 `kernel_vector` 保存 31 个 GPR 到内核栈，调用 `trap_kernel_handler()`。
- **S-mode（用户）**：`trampoline.S` 中的 `user_vector` 保存用户寄存器到 trapframe 并切换到内核页表；`user_return` 切换回用户页表并恢复用户寄存器。

### 进程管理

`proc_t` 结构包含：pid、用户页表、heap_top、用户栈页、trapframe 指针、内核栈地址、内核上下文。`swtch.S` 实现被调用者保存的上下文切换（ra, sp, s0-s11）。

### 用户空间

`initcode.c` 是第一个用户程序，编译后通过 `xxd` 转为 `initcode.h`（C 字节数组头文件）。`syscall_arch.h` 提供内联汇编 `ecall` 包装，系统调用号通过 a7 传递，当前仅有 `SYS_helloworld = 0`。

## 注意事项

- **`src/user/initcode.h` 是自动生成的**，不要手动编辑，`make build` 会重新生成
- Include 路径 `-I.` 使得 include 相对于项目根目录；内核内模块引用用 `"模块名/mod.h"`，跨目录用 `"../模块名/mod.h"`
- 用户代码编译额外加 `-march=rv64g -nostdinc`
- 没有测试框架；`mem/pmem.c` 中有一个 `test_case_2()` 物理内存测试函数

