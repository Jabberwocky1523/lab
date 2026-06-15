# LAB-8: 文件系统 之 数据组织与层次结构

**前言**

在lab-7中我们实现了block-level的磁盘管理能力

在此基础上, 本次实验将进一步考虑以下两个问题:

1. 如果数据块的大小大于1个block, 如何组织和管理? (inode)

2. 如何用人类更好理解的层次结构来组织和索引海量数据块? (dentry)

## 代码组织结构

```
ECNU-OSLAB-2025-TASK
├── LICENSE        开源协议
├── .vscode        配置了可视化调试环境
├── registers.xml  配置了可视化调试环境
├── .gdbinit.tmp-riscv xv6自带的调试配置
├── common.mk      Makefile中一些工具链的定义
├── Makefile       编译运行整个项目
├── kernel.ld      定义了内核程序在链接时的布局
├── picture        README使用的图片目录 (CHANGE)
├── README.md      实验指导书 (CHANGE)
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
    │   │   ├── utils.c (CHANGE, 新增strlen函数)
    │   │   ├── method.h (CHANGE)
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── mem    内存模块
    │   │   ├── pmem.c
    │   │   ├── kvm.c
    │   │   ├── uvm.c
    │   │   ├── mmap.c
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── trap   陷阱模块
    │   │   ├── plic.c
    │   │   ├── timer.c
    │   │   ├── trap_kernel.c
    │   │   ├── trap_user.c
    │   │   ├── trap.S
    │   │   ├── trampoline.S
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── proc   进程模块
    │   │   ├── proc.c
    │   │   ├── swtch.S
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── syscall 系统调用模块
    │   │   ├── syscall.c
    │   │   ├── sysfunc.c
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h
    │   ├── fs     文件系统模块
    │   │   ├── bitmap.c
    │   │   ├── buffer.c
    │   │   ├── inode.c (TODO, 核心工作)
    │   │   ├── dentry.c (TODO, 核心工作)
    │   │   ├── fs.c (TODO, 增加inode初始化逻辑和测试用例)
    │   │   ├── virtio.c
    │   │   ├── method.h (CHANGE)
    │   │   ├── mod.h
    │   │   └── type.h (CHANGE)
    │   └── main.c
    ├── mkfs       磁盘映像初始化
    │   ├── mkfs.c (CHANGE, 更复杂的文件系统初始化)
    │   └── mkfs.h (CHANGE)
    └── user       用户程序
        ├── initcode.c
        ├── sys.h
        ├── syscall_arch.h
        └── syscall_num.h
```
## 实现总结

### 已完成的 TODO 模块

#### 1. `src/kernel/fs/inode.c` — 索引节点管理

**数据块组织 (index tree)**
- `__free_data_blocks(block_num, level)` — 递归释放 index 树中的块资源，遇到空块号 (文件末尾) 立即返回
- `free_data_blocks(inode_index)` — 释放 inode 管理的所有块：直接映射 (index[0..9]) → 一级间接 (index[10..11]) → 二级间接 (index[12])
- `locate_or_add_block(inode_index, logical_block_num)` — 逻辑块号 → 物理块号转译：
  - 直接映射：10 个数据块 (40KB)
  - 一级间接：2 × 1024 个数据块 (8MB)
  - 二级间接：1 × 1024 × 1024 个数据块 (4GB)
  - 自动在边界处分配新块 (类似页表生长)

**inode 生命周期**
- `inode_init()` — 初始化 N_INODE=64 槽位的 inode_cache + 自旋锁 lk_inode_cache
- `inode_rw(ip, write)` — 内存 ↔ 磁盘 inode_region 双向同步
- `inode_get(inode_num)` — 从 cache 获取 (miss 时分配空闲槽位，ref++)
- `inode_create(type, major, minor)` — 分配 inode_num + 填充 disk_info + 写回磁盘
- `inode_dup(ip)` — ref++ (支持多使用者共享)
- `inode_lock(ip)` — 获取睡眠锁，必要时从磁盘读入 disk_info
- `inode_unlock(ip)` — 释放睡眠锁
- `inode_put(ip)` — ref--; 当 ref==1 && nlink==0 时触发 inode_delete
- `inode_delete(ip)` — 释放所有数据块 + 释放 inode bitmap

**数据流读写**
- `inode_read_data(ip, offset, len, dst, is_user_dst)` — 从 inode 管理的数据空间读取到内存
- `inode_write_data(ip, offset, len, src, is_user_src)` — 从内存写入 inode 管理的数据空间，自动扩展文件大小

#### 2. `src/kernel/fs/dentry.c` — 目录项与路径解析

**目录项槽位管理**
- `dentry_search(ip, name)` — 在目录的 index[0] 块中按名查找，返回 inode_num 或 INVALID_INODE_NUM
- `dentry_create(ip, inode_num, name)` — 寻找空闲槽位插入新目录项，重名返回 -1
- `dentry_delete(ip, name)` — 清除目标槽位 (name[0]=0)，返回被删目录项的 inode_num

**路径解析**
- `get_element(path, name)` — 提取路径中的下一级文件名 (跳过连续 `/`)
- `__path_to_inode(path, name, find_parent_inode)` — 从 ROOT_INODE 出发逐级遍历目录树：
  - `find_parent_inode=false` → 返回目标 inode
  - `find_parent_inode=true` → 返回父目录 inode + 文件名
- `path_to_inode(path)` / `path_to_parent_inode(path, name)` — 对外接口

#### 3. `src/kernel/fs/fs.c` — 文件系统初始化

- `fs_init()` 中添加 `inode_init()` 调用 (初始化 inode_cache)

### 实现中的关键设计点

**缓冲区旧数据清零**（在 `dentry.c` 中处理）：块被 `bitmap_free_block` 释放后重新分配时，buffer cache 可能残留旧数据。在 `dentry_create` 为新目录分配 `index[0]` 块时通过 `buffer_get` + `memset` + `buffer_write` + `buffer_put` 显式清零。

**LIFO 连续分配适配**（在 `fs.c` 测试2中处理）：`pmem_alloc` 使用 LIFO 空闲链表，连续 5 次分配返回降序地址。先分配到临时数组 `pages[5]` 验证连续性 (`pages[i] == pages[0] - i*PGSIZE`)，再取最低地址 `pages[4]` 作为 `big_src` 基址。

**inode 写同步**（在 `fs.c` 测试4中处理）：`inode_write_data` 只修改内存中的 `disk_info`，测试 4 中补充 `inode_rw(ip_3, true)` 将 file.txt 的 size/index 变更同步到磁盘，确保后续 `path_to_inode` 重新获取时读到最新数据。

**mkfs C23 兼容**（在 `mkfs.h` 中处理）：将自定义 `typedef enum { false, true } bool` 替换为 `#include <stdbool.h>`，解决新版 GCC 的 `false`/`true` 关键字冲突。

### 测试结果

4 个测试用例均通过 `make run` 验证：

- **测试1** ✅ — inode 创建/dup/删除，bitmap 状态转换正确
- **测试2** ✅ — 小数据 (16KB) 读写一致；大数据 (17KB quick mode) 跨块写入读取 `GHABCDEF`
- **测试3** ✅ — dentry 搜索预置文件 (ABCD.txt/abcd.txt)，内容读取正确；创建/删除 new_dir 正常
- **测试4** ✅ — 路径解析 `///AABBC///aaabb/file.txt` 正确，读取文件内容 `This is file context!`