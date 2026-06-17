# Lab-3 TODO

## 1. kvm.c — 内核页表补充映射

`kvm_init()` 缺少两项映射：

- **trampoline 页面映射**：将 `trampoline` 物理地址映射到 `TRAMPOLINE` 虚拟地址（`VA_MAX - PGSIZE`），权限 `PTE_R | PTE_X`
- **KSTACK(0) 映射**：为进程 0 分配内核栈物理页，映射到 `KSTACK(0)` 虚拟地址

文件：[src/kernel/mem/kvm.c](src/kernel/mem/kvm.c)

页表布局从低到高依次为 4KB空，code+data ，heap和栈区域 其中栈向下生长，heap向上, 最后是4kbtrapframe,4kbtrampoline。
---

## 2. proc.c — `proc_pgtbl_init()` 实现

创建并初始化用户态页表：

- [ ] 分配一个物理页作为用户态根页表
- [ ] 将 `trampoline` 页面映射到用户页表的 `TRAMPOLINE` 地址（同内核页表的映射）
- [ ] 将 `trapframe` 物理页映射到用户页表的 `TRAPFRAME` 地址，权限 `PTE_R | PTE_W | PTE_U`

文件：[src/kernel/proc/proc.c:22](src/kernel/proc/proc.c#L22)

---

## 3. proc.c — `proc_make_first()` 实现

创建第一个用户进程 `proczero`，用户地址空间布局（从高到低）：

```
TRAMPOLINE   (1 page)   ← 已在 proc_pgtbl_init 中映射
TRAPFRAME    (1 page)   ← 已在 proc_pgtbl_init 中映射
ustack       (1 page)   ← 分配物理页并映射
......
                     <-- heap_top（初始指向 code+data 末尾）
code + data  (1 page)   ← 将 initcode 拷贝到此
empty space  (1 page)   ← 最低 4KB guard page，不分配不映射
```

完成后设置 `proczero` 各字段（pid, pgtbl, heap_top, ustack_npage, tf, kstack, ctx），并将 `cpus[0].proc` 指向它。

文件：[src/kernel/proc/proc.c:42](src/kernel/proc/proc.c#L42)

---

## 4. trap_user.c — `trap_user_handler()` 实现

用户态 trap 入口处理（由 `trampoline.S:user_vector` 调用）：

- [ ] 读取 `scause` 判断 trap 类型
- [ ] **系统调用**（`ecall from U-mode`）：根据 `a7` 中的 syscall number 分发
  - `SYS_helloworld (0)`：`printf("helloworld\n")`
- [ ] **中断/异常**：打印错误信息并 panic
- [ ] 处理完成后 `sepc += 4`（指向下一条指令），调用 `trap_user_return()` 返回

文件：[src/kernel/trap/trap_user.c:17](src/kernel/trap/trap_user.c#L17)

---

## 5. trap_user.c — `trap_user_return()` 实现

从内核态返回用户态：

- [ ] 设置 `stvec` 指向 `user_vector`（用户态运行时 trap 走 trampoline 路径）
- [ ] 调用 trampoline 中的 `user_return(trapframe, pagetable)`，恢复用户寄存器并 `sret` 到 U-mode

文件：[src/kernel/trap/trap_user.c:24](src/kernel/trap/trap_user.c#L24)

---

## 依赖关系

```
kvm (trampoline映射) ──┐
                        ├──→ proc_make_first ──→ 第一个进程跑起来
proc_pgtbl_init ───────┘
                              │
                              ▼
                      initcode 执行 ecall
                              │
                              ▼
                    trap_user_handler ←── 处理 SYS_helloworld
                              │
                              ▼
                    trap_user_return ──→ 返回用户态
```

按 **1 → 2 → 3 → 4 → 5** 顺序完成。
