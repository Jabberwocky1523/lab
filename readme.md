# LAB-7: 文件系统 之 磁盘管理

**前言**

本次实验我们将围绕磁盘管理构建文件系统的基础设施

1. 首先讨论QEMU启动时的输入参数disk.img是如何构建的

2. 随后讨论以block为基本单位的磁盘读写如何实现, 包括驱动本身+OS提供的配合

3. 随后讨论磁盘与内存进行数据交换的桥梁--缓冲系统(buffer)

4. 最后讨论磁盘上bitmap区域的管理方法

## 代码组织结构

```
ECNU-OSLAB-2025-TASK
├── LICENSE        开源协议
├── .vscode        配置了可视化调试环境
├── registers.xml  配置了可视化调试环境
├── .gdbinit.tmp-riscv xv6自带的调试配置
├── common.mk      Makefile中一些工具链的定义
├── Makefile       编译运行整个项目 (CHANGE)
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
    │   │   ├── sleeplock.c
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
    │   │   ├── kvm.c (TODO, 内核页表增加磁盘相关映射 + vm_getpte处理pgtbl为NULL的情况)
    │   │   ├── uvm.c
    │   │   ├── mmap.c
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── trap   陷阱模块
    │   │   ├── plic.c (TODO, 增加磁盘中断相关支持)
    │   │   ├── timer.c
    │   │   ├── trap_kernel.c (TODO, 在外设处理函数中识别和响应磁盘中断)
    │   │   ├── trap_user.c
    │   │   ├── trap.S
    │   │   ├── trampoline.S
    │   │   ├── method.h
    │   │   ├── mod.h (CHANGE, include 文件系统模块)
    │   │   └── type.h
    │   ├── proc   进程模块
    │   │   ├── proc.c (在proc_return中调用文件系统初始化函数)
    │   │   ├── swtch.S
    │   │   ├── method.h
    │   │   ├── mod.h (CHANGE, include 文件系统模块)
    │   │   └── type.h
    │   ├── syscall 系统调用模块
    │   │   ├── syscall.c (TODO, 新增系统调用)
    │   │   ├── sysfunc.c (TODO, 新增系统调用)
    │   │   ├── method.h (CHANGE, 新增系统调用)
    │   │   ├── mod.h (CHANGE, include文件系统模块)
    │   │   └── type.h (CHANGE, 新增系统调用)
    │   ├── fs     文件系统模块
    │   │   ├── bitmap.c (TODO, bitmap相关操作)
    │   │   ├── buffer.c (TODO, 内存中的block缓冲区管理)
    │   │   ├── fs.c (TODO, 文件系统相关)
    │   │   ├── virtio.c (NEW, 虚拟磁盘的驱动)
    │   │   ├── method.h (NEW)
    │   │   ├── mod.h (NEW)
    │   │   └── type.h (NEW)
    │   └── main.c (CHANGE, 增加virtio_init)
    ├── mkfs       磁盘映像初始化
    │   ├── mkfs.c (NEW)
    │   └── mkfs.h (NEW)
    └── user       用户程序
        ├── initcode.c (CHANGE, 日常更新)
        ├── sys.h
        ├── syscall_arch.h
        └── syscall_num.h (CHANGE, 日常更新)
```

## LAB-7 实现总结

### 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `src/kernel/mem/kvm.c` | 移除 `kernel_pgtbl` 的 `static` 修饰符；在 `kvm_init()` 中添加 VIRTIO MMIO 地址映射；在 `vm_getpte()` 中处理 `pgtbl==NULL` 时使用内核页表 |
| `src/kernel/mem/method.h` | 添加 `extern pgtbl_t kernel_pgtbl;` 声明 |
| `src/kernel/fs/mod.h` | 添加 `#include "../mem/mod.h"` 使 virtio 驱动能访问 `vm_getpte` |
| `src/kernel/trap/plic.c` | `plic_init()` 中添加 VIRTIO_IRQ 优先级设置；`plic_inithart()` 中添加 VIRTIO_IRQ 使能 |
| `src/kernel/trap/trap_kernel.c` | `external_interrupt_handler()` 中添加 `virtio_disk_intr()` 调用分支 |
| `src/kernel/proc/proc.c` | `proc_return()` 中调用 `fs_init()`（仅首次）；用户栈从 1 页扩展到 4 页以支持测试用例的大数组 |
| `src/kernel/fs/fs.c` | 实现 `fs_init()`：初始化缓冲系统 → 读入超级块 → 验证魔数 → 输出磁盘布局 |
| `src/kernel/fs/buf.c` | 实现全部缓冲区管理函数：`buffer_init`、`buffer_read`、`buffer_write`、`buffer_get`（LRU 算法）、`buffer_put`、`buffer_freemem` |
| `src/kernel/fs/bitmap.c` | 实现全部位图操作：`bitmap_search_and_set`、`bitmap_clear`、`bitmap_alloc_block`、`bitmap_alloc_inode`、`bitmap_free_block`、`bitmap_free_inode` |
| `src/kernel/syscall/syscall.c` | 跳转表中新增 11 个系统调用条目（SYS_alloc_block ~ SYS_flush_buffer） |
| `src/kernel/syscall/sysfunc.c` | 实现 11 个新系统调用的服务函数 |
| `src/kernel/fs/type.h` | `N_BUFFER` 改为 `N_BUFFER_TEST`（测试模式） |
| `src/user/initcode.c` | 完成三个测试用例（test-1 默认激活，test-2/test-3 注释备用） |
| `Makefile` | 添加 `FORCE` 伪目标使 `disk.img` 每次构建时重新生成 |

### 关键设计决策

1. **vm_getpte 处理 NULL 页表**：当 `pgtbl == NULL` 时自动使用内核页表 `kernel_pgtbl`，这允许 `virtio_disk_rw` 通过 `vm_getpte(NULL, addr, false)` 将内核栈上的虚拟地址翻译为物理地址，用于 DMA 描述符。

2. **LRU 缓冲管理**：采用双链表设计——活跃链表（ref > 0）和非活跃链表（ref == 0）。
   - **缓存命中（活跃链表）**：移动到活跃链表头部（最活跃位置）
   - **缓存命中（非活跃链表）**：移动到活跃链表头部，若数据页被释放则重新读取
   - **缓存未命中**：取非活跃链表尾部（最不活跃），移动到活跃链表尾部

3. **fs_init 调用时机**：在 `proc_return()` 中首次被调度时调用（而非 `main()` 中），因为磁盘 I/O 需要进程上下文支持 `proc_sleep`/`proc_wakeup`。调用前先释放进程锁以避免死锁。

4. **位图管理**：基于 buffer 层实现，每次修改后调用 `buffer_write` 同步到磁盘。`bitmap_search_and_set` 采用逐字节遍历 + 位运算，支持最后一个块的部分有效范围。

### 测试结果验证

**test-1（超级块读取）**：成功输出磁盘布局信息，验证了缓冲系统和磁盘驱动的基本能力。

**test-2（位图操作）**：
- 分配 20 个 data block → 位图显示 1067~1086 ✓
- 释放偶数索引 10 个 → 位图显示剩余 10 个 ✓
- 释放奇数索引 10 个 → 位图为空 ✓
- 分配 20 个 inode → 位图显示 0~19 ✓
- 释放全部 20 个 inode → 位图为空 ✓

**test-3（缓冲区 LRU）**：
- 写入 "ABCDEFGH\n" 到 block 5000 → 刷新 → 读回 → 数据一致 ✓
- GET 五个不同 block → 活跃链表正确反映 LRU 顺序 ✓
- PUT 三个 buffer → 非活跃链表正确反映释放顺序 ✓
- FLUSH 3 → 非活跃链表尾部 3 个 buffer 的数据页被释放 ✓
