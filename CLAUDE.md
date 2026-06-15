# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 在本仓库中工作提供指导。

## 构建与运行

```bash
make build          # 构建内核 ELF + 用户 initcode → target/
make run            # 构建并在 qemu-system-riscv64 中运行
make debug          # 构建并启动 QEMU，挂载 GDB stub
make clean          # 删除 target/ 及生成文件
```

交叉编译工具链：`riscv64-linux-gnu-{gcc,ld,objcopy,objdump}`。工具链配置见 [common.mk](common.mk)。

GDB 端口动态计算：`$(id -u) % 5000 + 25000`。`make debug` 从 `.gdbinit.tmpl-riscv` 自动生成 `.gdbinit` 并填入正确端口。

## 架构概览

这是一个 **RISC-V 64 位教学操作系统内核**（ECNU OSLAB），参考 xv6 设计。运行在 `qemu-system-riscv64` 上，SV39 虚拟内存，支持多核（NCPU=2）。

### 启动流程

```
M-mode: entry.S → start.c → mret → S-mode: main()
```

- [src/kernel/boot/start.c](src/kernel/boot/start.c) 在 M-mode 运行：将 hartid 存入 `tp`，通过 `medeleg`/`mideleg` 委托所有 trap 给 S-mode，初始化 M-mode 时钟，配置 PMP 允许全部物理内存访问，然后 `mret` 到 `main()`。
- [src/kernel/main.c](src/kernel/main.c)：CPU0 依次初始化 `pmem_init → mmap_init → kvm_init → kvm_inithart → trap_kernel_init → trap_kernel_inithart → proc_make_first`，然后 `started=1`。其他 CPU 自旋等待 `started` 后初始化自身的页表和 trap 向量。

### 内存模型 (SV39)

物理内存布局（[kernel.ld](kernel.ld)）：
- `0x80000000` — 内核 `.text`（代码段）
- trampoline 页通过 `trampsec` section 嵌入 `.text`，对齐到一页
- `KERNEL_DATA` — `.rodata`、`.data`、`.bss`
- `ALLOC_BEGIN` 至 `0x80000000 + 128M` — 空闲内存（前 `KERN_PAGES`=1024 页归内核，其余归用户）

虚拟地址空间常量（[src/kernel/mem/type.h](src/kernel/mem/type.h)）：

| 常量 | 地址 | 用途 |
|---|---|---|
| `TRAMPOLINE` | `VA_MAX - PGSIZE` | 内核↔用户切换代码（两个页表恒等映射） |
| `TRAPFRAME` | `TRAMPOLINE - PGSIZE` | 寄存器保存/恢复区（仅用户页表） |
| `KSTACK(n)` | `TRAPFRAME - (n+1)*2*PGSIZE` | 每进程内核栈（仅内核页表） |
| `MMAP_END` | `TRAPFRAME - 16*256*PGSIZE` | mmap 区域上限（给栈留 16MB） |
| `MMAP_BEGIN` | `MMAP_END - 64*256*PGSIZE` | mmap 区域下限（单个进程最多 64MB） |
| `USER_BASE` | `PGSIZE` (0x1000) | 用户代码/数据起始；第 0 页为 guard page |

SV39 三级页表：`VA[VPN2:VPN1:VPN0:offset]`（9:9:9:12 位）。核心函数 `vm_getpte`、`vm_mappages`、`vm_unmappages` 在 [src/kernel/mem/kvm.c](src/kernel/mem/kvm.c)。

### 物理内存分配器

[src/kernel/mem/pmem.c](src/kernel/mem/pmem.c) — 空闲页组成单链表，利用空闲页前 8 字节作 `next` 指针。`pmem_alloc(true)` 分配内核内存（前 1024 页），`pmem_alloc(false)` 分配用户内存。`check_inkernel(pa)` 根据物理地址判断属于内核还是用户区域。

### Trap 处理

**两阶段设计**：trampoline 汇编处理特权级跨越，C handler 包含业务逻辑。

- **内核 trap**（[src/kernel/trap/trap_kernel.c](src/kernel/trap/trap_kernel.c)）：`kernel_vector`（[trap.S](src/kernel/trap/trap.S)）保存上下文 → `trap_kernel_handler()`。处理时钟中断（仅 CPU0 更新 tick）和 PLIC 外设中断（UART）。
- **用户 trap**（[src/kernel/trap/trap_user.c](src/kernel/trap/trap_user.c)）：
  - `ecall`（scause=8）→ `syscall()` 分发器，从 `p->tf->a7` 读系统调用号，跳转表索引处理函数，`user_to_kern_epc += 4` 跳过 ecall
  - Load/Store page fault（scause=13/15）→ 尝试 `uvm_ustack_grow` 自动扩展栈，成功则返回用户态重试
- **stvec 切换**：内核态 → `kernel_vector`；返回用户态前 → `TRAMPOLINE + (user_vector - trampoline)`

### 系统调用模块

[src/kernel/syscall/](src/kernel/syscall/) — LAB-5 新增。

- `syscall()`（[syscall.c](src/kernel/syscall/syscall.c)）：跳转表分发，支持 `SYS_MAX_NUM=6` 个系统调用。
- 参数读取：`arg_raw(n)` 从 trapframe 的 `a0`-`a5` 读取；`arg_str(n, buf, maxlen)` 通过 `uvm_copyin_str` 从用户空间拷贝字符串。
- 当前系统调用（[sysfunc.c](src/kernel/syscall/sysfunc.c)）：

| 系统调用 | 功能 |
|---|---|
| `SYS_copyin` (1) | 用户→内核数据拷贝 |
| `SYS_copyout` (2) | 内核→用户数据拷贝 |
| `SYS_copyinstr` (3) | 用户→内核字符串拷贝 |
| `SYS_brk` (4) | 堆伸缩（0=查询，>heap_top=增长，<heap_top=收缩） |
| `SYS_mmap` (5) | 创建内存映射（begin=0 时自动寻找空隙） |
| `SYS_munmap` (6) | 解除内存映射 |

### 用户态虚拟内存管理

[src/kernel/mem/uvm.c](src/kernel/mem/uvm.c) — LAB-5 新增，用户态虚拟内存核心。

- **数据迁移**：`uvm_copyin` / `uvm_copyout` / `uvm_copyin_str` — 内核↔用户空间拷贝，支持非页对齐地址，逐页查找 PTE 获取物理地址后逐字节拷贝。
- **堆管理**：`uvm_heap_grow` / `uvm_heap_ungrow` — 按页分配/释放，上限不超过 `MMAP_BEGIN`，下限不低于 `USER_BASE + PGSIZE`。
- **栈自动扩展**：`uvm_ustack_grow` — 由 page fault 驱动，验证 fault_addr 在合法范围内后一次性扩展到位，返回新页数。
- **mmap/munmap**：`uvm_mmap` 支持 First-Fit 自动找空隙 + 相邻区域合并 + 有序链表插入；`uvm_munmap` 处理六种情况（完全删除、中间分割、左侧截断、右侧截断等）。
- **页表管理**：`uvm_copy_pgtbl` 复制代码/栈/mmap 区域；`uvm_destroy_pgtbl` 递归释放页表及叶子物理页（trampoline 不释放，trapframe 单独释放）。

### mmap_region_node 资源仓库

[src/kernel/mem/mmap.c](src/kernel/mem/mmap.c) — 全局数组 `node_list[256]` + 链表头 + 自旋锁。`mmap_region_alloc()` 加锁取出头节点返回内嵌的 `mmap_region_t*`；`mmap_region_free()` 通过指针运算验证合法性后归还链表。仓库空则 panic。

### 进程模型

[src/kernel/proc/proc.c](src/kernel/proc/proc.c) — `proc_t` 包含：页表、堆顶、栈页数、mmap 链表、trapframe、内核栈、上下文。

`proc_make_first()` 创建 `proczero`（静态分配）：
1. `mmap = NULL` 初始化
2. 分配 trapframe 页（内核内存）
3. `proc_pgtbl_init` 创建用户页表（trampoline + trapframe 映射）
4. 内核栈 = `KSTACK(0)`（物理页已在 `kvm_init` 分配）
5. 分配 1 页用户栈（`TRAPFRAME - PGSIZE`）
6. 加载 initcode 到 `USER_BASE`（从 `initcode.h` 数组拷贝）
7. `mycpu()->proc = &proczero`；`trap_user_return()` 切入 U-mode（不返回）

内核线程上下文切换：`swtch(old_ctx, new_ctx)`（[swtch.S](src/kernel/proc/swtch.S)）仅保存/恢复 callee-saved 寄存器。

### 用户程序

[src/user/initcode.c](src/user/initcode.c) — 编译为裸二进制 → `xxd -i` 生成 `initcode.h`（C 数组）→ 链接进内核。当前测试 mmap/munmap 的合并与分割逻辑。

### 模块约定

每个子系统遵循 `method.h` / `mod.h` / `type.h` 模式：
- `type.h` — 结构体和常量定义
- `method.h` — 函数声明
- `mod.h` — 按依赖顺序引入（`type.h` → 依赖模块的 `mod.h` → 本模块 C 头文件 → `method.h`）

### 关键 RISC-V CSR

- `satp` — 页表根 + SV39 模式；`stvec` — trap 处理函数地址
- `sstatus` — `SPP`（之前的特权级）、`SPIE`/`SIE` 中断使能
- `sepc` — trap 时的 PC；`scause` — trap 原因；`stval` — trap 辅助信息（缺页地址等）
- `sscratch` — trampoline 保存寄存器时暂存 `a0`
- `tp` — 存放 hartid（启动时设置）
