# LAB-5: 系统调用流程建立 + 用户态虚拟内存管理

## 代码组织结构

```
ECNU-OSLAB-2025-TASK
├── LICENSE        开源协议
├── .vscode        配置了可视化调试环境
├── registers.xml  配置了可视化调试环境
├── .gdbinit.tmp-riscv xv6自带的调试配置
├── common.mk      Makefile中一些工具链的定义
├── Makefile       编译运行整个项目 (CHANGE, 新增目录syscall)
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
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
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
    │   │   ├── kvm.c
    │   │   ├── uvm.c (TODO, 用户态虚拟内存管理主体)
    │   │   ├── mmap.c (TODO, mmap节点资源仓库)
    │   │   ├── method.h (CHANGE, 日常更新)
    │   │   ├── mod.h
    │   │   └── type.h (CHANGE, 日常更新)
    │   ├── trap   陷阱模块
    │   │   ├── plic.c
    │   │   ├── timer.c
    │   │   ├── trap_kernel.c
    │   │   ├── trap_user.c (TODO, 系统调用处理 + pagefault处理)
    │   │   ├── trap.S
    │   │   ├── trampoline.S
    │   │   ├── method.h
    │   │   ├── mod.h (CHANGE, 日常更新)
    │   │   └── type.h
    │   ├── proc   进程模块
    │   │   ├── proc.c (TODO, proczero->mmap初始化)
    │   │   ├── swtch.S
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h (CHANGE, 进程结构体里新增mmap字段)
    │   ├── syscall 系统调用模块
    │   │   ├── syscall.c (NEW, 系统调用通用逻辑)
    │   │   ├── sysfunc.c (TODO, 各个系统调用的处理逻辑) 
    │   │   ├── method.h (NEW)
    │   │   ├── mod.h (NEW)
    │   │   └── type.h (NEW)
    │   └── main.c
    └── user       用户程序
        ├── initcode.c (CHANGE, 按照测试需求来设置)
        ├── sys.h
        ├── syscall_arch.h
        └── syscall_num.h (CHANGE, 日常更新)
```

---

## LAB-5 实现总结

### 任务1：用户态和内核态的数据迁移

在 `src/kernel/mem/uvm.c` 中实现了三个用户态-内核态数据传递函数：

- **`uvm_copyin(pgtbl, dst, src, len)`**：从用户态地址空间 `[src, src+len)` 拷贝到内核态地址空间 `[dst, dst+len)`。通过逐页查询用户页表获取物理地址，使用 `memmove` 完成数据迁移，支持非页对齐的地址和长度。
- **`uvm_copyout(pgtbl, dst, src, len)`**：从内核态拷贝到用户态，与 `uvm_copyin` 方向相反，原理相同。
- **`uvm_copyin_str(pgtbl, dst, src, maxlen)`**：从用户态拷贝字符串到内核态，遇到 `'\0'` 终止，最多拷贝 `maxlen` 字节；若超长未找到终止符则 panic。

在 `src/kernel/trap/trap_user.c` 中将系统调用处理（case 8）从内联的 `SYS_helloworld` 改为调用 `syscall()` 分发器，支持通过系统调用表进行跳转。

在 `src/kernel/syscall/sysfunc.c` 中实现了三个对应的系统调用处理函数：
- **`sys_copyout`**：将内核中的 `{1, 2, 3, 4, 5}` 数组发送到用户空间
- **`sys_copyin`**：从用户空间读取数组并打印
- **`sys_copyinstr`**：从用户空间读取字符串并打印

**测试结果**：`sys_copyout → sys_copyin → sys_copyinstr` 依次执行，正确输出 "1 2 3 4 5" 和 "hello, world"。

---

### 任务2：堆的手动管理与栈的自动管理

**堆管理** — 在 `src/kernel/mem/uvm.c` 中实现：

- **`uvm_heap_grow(pgtbl, cur_heap_top, len)`**：将堆顶从 `cur_heap_top` 扩展到 `cur_heap_top + len`。边界检查确保不超过 `MMAP_BEGIN`；按页分配物理内存（`pmem_alloc(false)`）并建立用户页表映射（`PTE_R | PTE_W | PTE_U`）。
- **`uvm_heap_ungrow(pgtbl, cur_heap_top, len)`**：将堆顶缩减到 `cur_heap_top - len`，最低不低于 `USER_BASE + PGSIZE`；释放多余的物理页并解除映射。

在 `src/kernel/syscall/sysfunc.c` 中实现 **`sys_brk`**：
- `new_heap_top == 0`：查询当前堆顶
- `new_heap_top > old`：调用 `uvm_heap_grow` 扩展
- `new_heap_top < old`：调用 `uvm_heap_ungrow` 收缩
- `new_heap_top == old`：不操作
- 每次都输出调试信息（旧堆顶、新堆顶、操作结果）

**测试结果**：查询返回 0x2000 → 扩展9页到 0xB000 → 不变 → 缩减5页到 0x6000。

**栈管理** — 在 `src/kernel/mem/uvm.c` 中实现：

- **`uvm_ustack_grow(pgtbl, old_ustack_npage, fault_addr)`**：处理缺页异常驱动的栈自动扩展。
  - 确认 `fault_addr` 在当前栈底下方（栈向低地址增长）
  - 确认 `ALIGN_DOWN(fault_addr, PGSIZE) >= MMAP_END`（不越界到 mmap 区域）
  - 计算需要的新页数，一次性扩展到位
  - 分配物理页并映射，返回新的 `ustack_npage`

在 `src/kernel/trap/trap_user.c` 中更新了缺页异常处理（case 13/15）：
- 读取 `stval` 获取故障地址
- 打印调试信息（ustack_npage、当前栈底）
- 调用 `uvm_ustack_grow` 尝试扩展
- 成功则 `break` 返回用户态重试；失败则 panic

**测试结果**：`char tmp[PGSIZE * 4]` 声明触发两次缺页异常：
- 第一次 Store Page Fault：栈从 1 页扩展到 2 页（编译器序言保存寄存器）
- 第二次 Store Page Fault：栈从 2 页扩展到 5 页（`tmp[0] = 'w'` 访问）
- 两次 `sys_copyinstr` 分别正确输出 "hello" 和 "world"

---

### 任务3：mmap_region_node 仓库管理

在 `src/kernel/mem/mmap.c` 中实现了受自旋锁保护的 `mmap_region_node` 资源仓库：

- **`mmap_init()`**：将全局数组 `node_list[N_MMAP]`（256个节点）链接成单向空闲链表，`list_head.next` 指向第一个节点。初始化自旋锁 `list_lk`。
- **`mmap_region_alloc()`**：加锁从链表头取出一个节点，返回其内嵌的 `mmap_region_t*` 指针；链表空则 panic。
- **`mmap_region_free(mmap)`**：通过指针运算（`node - node_list`）验证指针合法性，加锁将节点插入链表头归还。
- **`mmap_show_nodelist()`**：加锁遍历链表，打印每个节点的编号和数组索引（供调试）。

在 `src/kernel/main.c` 的 CPU 0 启动流程中添加了 `mmap_init()` 调用，确保仓库在任何系统调用使用前初始化。

在 `src/kernel/proc/proc.c` 的 `proc_make_first()` 中添加了 `p->mmap = NULL` 初始化。

**测试结果**：仓库初始状态输出 `node 0 index = 0` 到 `node 255 index = 255`。多核并发申请/释放时，自旋锁保证操作的原子性和有序性。

---

### 任务4：mmap 与 munmap

在 `src/kernel/mem/uvm.c` 中实现了离散内存映射的核心逻辑：

- **`uvm_mmap_find(head_mmap, len, &last, &tmp)`**：当 `begin == 0` 时，从头扫描 mmap 链表，找到第一个能容纳 `len` 字节的空隙（First-Fit 策略）。从 `MMAP_BEGIN` 开始，依次检查每个区域之间的空隙以及最后一个区域到 `MMAP_END` 的空间。

- **`uvm_mmap(begin, npages, perm)`**：
  1. 若 `begin == 0`，调用 `uvm_mmap_find` 自动寻找位置
  2. 边界检查和重叠检查
  3. 从仓库申请 `mmap_region_t`，按 `begin` 有序插入进程的 mmap 链表
  4. 尝试与前驱合并（`prev->begin + prev->npages*PGSIZE == new->begin`）
  5. 尝试与后继合并（`new->begin + new->npages*PGSIZE == next->begin`）
  6. 为每个页面分配物理内存并建立映射
  7. 返回实际的 `begin` 地址

- **`uvm_munmap(begin, npages)`**：处理六种情况：
  - 区域完全在释放范围之前：跳过
  - 区域完全在释放范围之后：结束（链表有序）
  - 区域完全在释放范围内：整区删除，归还 `mmap_region_t` 到仓库
  - 释放范围在区域中间：区域一分为二（从仓库申请新节点）
  - 区域向左超出：截断右侧
  - 区域向右超出：截断左侧
  - 每种情况都正确维护链表结构和释放物理页面

- **`mmap_merge(mmap_1, mmap_2, keep_mmap_1)`**（辅助函数）：合并两个相邻区域，保留一个释放另一个，不操作 `next` 指针（由调用者处理链表连接关系）。

在 `src/kernel/syscall/sysfunc.c` 中实现了 **`sys_mmap`** 和 **`sys_munmap`**，包含参数合法性检查（len 必须页对齐且非零），每次操作后输出 mmap 链表状态和页表内容。

**测试结果**（7 次 mmap + 7 次 munmap）：
- mmap 序列正确完成了区域合并：`[4,7) → [4,7),[10,12) → [2,7),[10,12) → [2,13) → [0,13) → [0,23)`
- munmap 序列正确完成了区域分割和删除：`[0,23) → [0,10),[15,23) → [15,23) → [15,17),[19,23) → [19,23) → [21,23) → [21,22) → empty`

---

### 任务5：页表的复制与销毁

在 `src/kernel/mem/uvm.c` 中实现：

- **`destroy_pgtbl(pgtbl, level)`**（递归函数）：
  - 遍历页表的 512 个 PTE
  - 若 PTE 有效且为页表指针（`PTE_CHECK` 为真）且 `level > 1`：递归进入下级页表
  - 若 PTE 有效且为叶子页（`level == 1`）：释放对应物理页（`pmem_free` 配合 `check_inkernel` 自动识别内核/用户内存）
  - 递归返回后释放当前页表页本身

- **`uvm_destroy_pgtbl(pgtbl)`**：先单独处理 `TRAPFRAME`（释放，各进程独有）和 `TRAMPOLINE`（不释放，所有进程共享），再调用 `destroy_pgtbl(pgtbl, 3)` 递归释放剩余部分。

- **`copy_range(old, new, begin, end)`**（辅助函数）：逐页复制 `[begin, end)` 范围：为新页表分配物理页、拷贝数据内容、建立相同权限的映射。

- **`uvm_copy_pgtbl(old, new, heap_top, ustack_npage, mmap)`**：分别复制代码+数据区（`USER_BASE` ∼ `heap_top`）、栈区（`TRAPFRAME - ustack_npage*PGSIZE` ∼ `TRAPFRAME`）、所有 mmap 区域。不包括 trapframe 和 trampoline（由调用者处理）。

### 修改文件清单

| 文件                           | 修改类型        | 说明                                             |
| ------------------------------ | --------------- | ------------------------------------------------ |
| `src/kernel/mem/uvm.c`         | TODO → 完整实现 | 用户态虚拟内存管理核心                           |
| `src/kernel/mem/mmap.c`        | TODO → 完整实现 | mmap 资源仓库                                    |
| `src/kernel/syscall/sysfunc.c` | TODO → 完整实现 | 6 个系统调用处理函数                             |
| `src/kernel/trap/trap_user.c`  | TODO → 更新     | 系统调用分发 + 缺页处理                          |
| `src/kernel/proc/proc.c`       | TODO → 更新     | mmap 字段初始化                                  |
| `src/kernel/main.c`            | CHANGE → 更新   | 添加 mmap_init() 调用                            |
| `src/kernel/mem/method.h`      | CHANGE → 更新   | 添加 check_inkernel 声明，修改 uvm_mmap 返回类型 |