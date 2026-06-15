# LAB-9: 文件系统 之 文件管理与全系统整合

**前言**

恭喜你完成了前8次实验, 来到最后一个关卡

最后一个实验的内容比较多, 难度也比较大, 既是实验也是测验

- 测验你对整个系统的理解：内存、进程、文件系统、用户态程序等

- 测验你的编码与调试能力：文件操作、路径操作、ELF解析、系统调用等

不用担心, 助教会屏蔽大部分繁琐但不重要的工作, 并梳理实验脉络

这大概要花费你好几天时间, 现在就开始吧!

## 代码组织结构

```
ECNU-OSLAB-2025-TASK
├── LICENSE        开源协议
├── .vscode        配置了可视化调试环境
├── registers.xml  配置了可视化调试环境
├── .gdbinit.tmp-riscv xv6自带的调试配置
├── common.mk      Makefile中一些工具链的定义
├── Makefile       编译运行整个项目 (CHANGE)
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
    │   │   ├── console.c (NEW, 行缓冲的输入输出)
    │   │   ├── print.c (CHANGE, 在print_init中调用console_init进行初始化)
    │   │   ├── uart.c (CHANGE, 将uart_intr中的switch-case逻辑换成cons_edit)
    │   │   ├── utils.c
    │   │   ├── method.h (CHANGE)
    │   │   ├── mod.h
    │   │   └── type.h (CHANGE)
    │   ├── mem    内存模块
    │   │   ├── pmem.c (TODO, 增加函数pmem_stat用于获取剩余页面数量信息)
    │   │   ├── kvm.c
    │   │   ├── uvm.c (TODO, 修改uvm_heap_grow以支持flag的输入)
    │   │   ├── mmap.c
    │   │   ├── method.h (CHANGE)
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
    │   │   ├── proc.c (TODO, 增加open_file和cwd的初始化、设置、销毁逻辑)
    │   │   ├── exec.c (TODO, 操作ELF文件以填充新的进程)
    │   │   ├── swtch.S
    │   │   ├── method.h (CHANGE)
    │   │   ├── mod.h
    │   │   └── type.h (CHANGE)
    │   ├── syscall 系统调用模块
    │   │   ├── syscall.c (TODO, 新的系统调用)
    │   │   ├── sysfunc.c (TODO, 新的系统调用)
    │   │   ├── method.h (TODO, 新的系统调用)
    │   │   ├── mod.h
    │   │   └── type.h (TODO, 新的系统调用)
    │   ├── fs     文件系统模块
    │   │   ├── bitmap.c
    │   │   ├── buffer.c
    │   │   ├── inode.c
    │   │   ├── device.c (TODO, 增加设备文件操作逻辑)
    │   │   ├── dentry.c (TODO, 增加目录和路径的功能)
    │   │   ├── fs.c (TODO, 增加文件操作逻辑)
    │   │   ├── virtio.c
    │   │   ├── method.h (CHANGE)
    │   │   ├── mod.h
    │   │   └── type.h (CHANGE)
    │   └── main.c
    ├── mkfs       磁盘映像初始化
    │   ├── mkfs.c (CHANGE, 增加输入参数的支持)
    │   └── mkfs.h (CHANGE)
    ├── loader     存放链接脚本
    │   ├── kernel.ld (CHANGE, 移动了位置)
    │   └── user.ld (NEW, 定义了用户态ELF程序的链接规则)
    └── user       用户程序
        ├── initcode.c (CHANGE, 启动测试程序)
        ├── syscall.c (NEW, 封装了系统调用)
        ├── help.c (NEW, 其他公共库函数)
        ├── test_1.c (NEW, 测试点)
        ├── test_2.c (NEW, 测试点)
        ├── test_3.c (NEW, 测试点)
        ├── test_4.c (NEW, 测试点)
        ├── help.h (NEW, 库函数和重要定义)
        ├── sys.h
        ├── syscall_arch.h
        └── syscall_num.h (CHANGE, 新的系统调用)
```

# LAB-9 实验完成记录

## 实现概览

本次实验按照 readme 中的实验步骤，参考 xv6-labs-2020 的代码思路，逐步完成了文件系统的文件管理功能与全系统整合，包括文件操作、设备文件、目录管理、硬链接、ELF文件执行、系统调用等。

## 第1步：准备工作

### pmem_stat（[pmem.c](src/kernel/mem/pmem.c)）
- 读取 `kern_region.allocable` 和 `user_region.allocable` 并输出到指针参数
- 该函数被 `/dev/gpt0` 设备用于查询空闲内存

### uvm_heap_grow（[uvm.c](src/kernel/mem/uvm.c)）
- 修改函数签名，增加 `flag` 参数以支持自定义页面权限
- `sys_brk` 调用时传入 `PTE_R | PTE_W`，`exec.c` 的 `prepare_heap` 传入 `PTE_R | PTE_X`

### 控制台与UART
- [print.c](src/kernel/lib/print.c)：在 `print_init` 中调用 `cons_init()` 初始化控制台
- [uart.c](src/kernel/lib/uart.c)：将 `uart_intr` 中的字符回显逻辑替换为 `cons_edit(c)`

## 第2步：完善文件系统

### dentry.c（6个函数）
| 函数                | 功能                                               |
| ------------------- | -------------------------------------------------- |
| `dentry_search_2`   | 在目录中同时按 inode_num 和 name 查找              |
| `dentry_transmit`   | 将有效目录项传输到用户态/内核态缓冲区              |
| `inode_to_path`     | 逆向解析 inode 到绝对路径，利用 `..` 回溯          |
| `path_create_inode` | 基于路径创建新 inode，目录类型自动创建 `.` 和 `..` |
| `path_link`         | 建立硬链接（nlink++ 和目录项创建）                 |
| `path_unlink`       | 解除硬链接并处理资源释放                           |

### device.c（4个函数）
| 函数                | 功能                                                 |
| ------------------- | ---------------------------------------------------- |
| `device_init`       | 注册6种设备并确保 `/dev/` 目录及设备文件在磁盘中存在 |
| `device_open_check` | 检查设备文件的 major 号和打开权限合法性              |
| `device_read_data`  | 根据 major 号分发到对应设备的读函数                  |
| `device_write_data` | 根据 major 号分发到对应设备的写函数                  |

### fs.c（9个函数 + fs_init）
- `file_init/alloc/open/close/read/write/lseek/dup/get_stat` — 完整的文件生命周期管理
- `file_read/file_write` 按 inode 类型（DATA/DIR/DEVICE）做 switch-case 分发
- `fs_init` 中依次调用 `buffer_init → inode_init → 读超级块 → file_init → device_init`

## 第3步：进程与文件系统协作

### proc.c
- `proc_init`：初始化 `open_file[]` 为 NULL，`cwd` 为 NULL
- `proc_return`：为 proczero 打开 stdin/stdout/stderr，设置 cwd 为根目录
- `proc_fork`：子进程继承父进程的 `open_file`（file_dup）和 `cwd`（inode_dup）
- `proc_free`：释放所有 `open_file`（file_close）和 `cwd`（inode_put）

### 相对路径支持（dentry.c）
- 修改 `__path_to_inode`：不以 `/` 开头时从 `myproc()->cwd` 开始解析
- 支持 `.`（当前目录）和 `..`（父目录）语义

## 第4步：ELF文件执行

### exec.c — proc_exec
完整实现了 8 步 ELF 加载流程：
1. 准备全新的 pagetable 和 trapframe
2. 解析文件路径获取 ELF inode
3. 读取 ELF header 并验证 magic
4. 调用 `prepare_heap` 载入 program segments
5. 处理 `argv` 参数到用户栈（`prepare_stack`）
6. 释放旧资源（页表、mmap）
7. 设置 trapframe 的 entry、sp、a0（argc）、a1（argv）
8. 更新进程名称和堆栈信息

## 第5步：系统调用

### syscall.c
- 补全跳转表：SYS_exec(9) 到 SYS_unlink(22)，共 14 个新系统调用

### sysfunc.c（14个新系统调用）
`sys_exec`、`sys_open`、`sys_close`、`sys_read`、`sys_write`、`sys_lseek`、`sys_dup`、`sys_fstat`、`sys_get_dentries`、`sys_mkdir`、`sys_chdir`、`sys_print_cwd`、`sys_link`、`sys_unlink`

## 关键Bug修复

1. **目录 inode 写回缺失**：`path_create_inode` 创建目录后未将子目录 inode 的 `index[0]` 写回磁盘，导致后续查找丢失数据块。修复：在 `dentry_create` 后增加 `inode_rw(ip, true)` 调用。

2. **用户态地址直接访问**：`inode_read_data` / `inode_write_data` 虽有 `is_user_dst`/`is_user_src` 参数但未使用，直接用 `memmove` 访问用户地址导致 page fault。修复：使用 `uvm_copyout` / `uvm_copyin`。

3. **mkfs 间接映射**：用户程序 ELF 文件超过 40KB（10个直接映射块），需要实现一级间接映射支持。

4. **工具链兼容**：添加 `-std=gnu11` 编译标志，修复 `bool` 类型冲突，添加缺失的 `console_t` 类型定义和 `CONSOLE_INPUT_BUF` 常量。

## 测试结果

| 测试程序 | 结果 | 说明                                                                                                        |
| -------- | ---- | ----------------------------------------------------------------------------------------------------------- |
| test_1   | ✅    | 基本I/O、参数传递、exec传参正确                                                                             |
| test_2   | ✅    | open/close/dup/fstat + read/write/lseek/get_dentries 全部通过（500次fprintf循环在QEMU仿真下较慢但正确执行） |
| test_3   | ✅    | mkdir、chdir、print_cwd、link、unlink 全部通过                                                              |
| test_4   | ✅    | /dev/zero、/dev/null、/dev/gpt0 设备文件操作正常                                                            |

## 修改文件清单

### TODO函数实现（核心逻辑）
- [src/kernel/mem/pmem.c](src/kernel/mem/pmem.c)
- [src/kernel/mem/uvm.c](src/kernel/mem/uvm.c)
- [src/kernel/fs/dentry.c](src/kernel/fs/dentry.c)
- [src/kernel/fs/device.c](src/kernel/fs/device.c)
- [src/kernel/fs/fs.c](src/kernel/fs/fs.c)
- [src/kernel/fs/inode.c](src/kernel/fs/inode.c)
- [src/kernel/proc/proc.c](src/kernel/proc/proc.c)
- [src/kernel/proc/exec.c](src/kernel/proc/exec.c)
- [src/kernel/syscall/syscall.c](src/kernel/syscall/syscall.c)
- [src/kernel/syscall/sysfunc.c](src/kernel/syscall/sysfunc.c)

### 辅助修复
- [src/kernel/arch/type.h](src/kernel/arch/type.h) — 取消 bool 注释
- [src/kernel/lib/type.h](src/kernel/lib/type.h) — 添加 console_t、CONSOLE_INPUT_BUF
- [src/kernel/lib/print.c](src/kernel/lib/print.c) — 调用 cons_init
- [src/kernel/lib/uart.c](src/kernel/lib/uart.c) — 使用 cons_edit
- [src/kernel/mem/method.h](src/kernel/mem/method.h) — 添加声明
- [src/kernel/fs/method.h](src/kernel/fs/method.h) — 添加声明
- [src/user/help.c](src/user/help.c) — 添加 memcpy
- [src/user/help.h](src/user/help.h) — 添加 memcpy 声明
- [src/user/initcode.c](src/user/initcode.c) — 调整测试路径
- [src/mkfs/mkfs.c](src/mkfs/mkfs.c) — 间接映射支持
- [src/mkfs/mkfs.h](src/mkfs/mkfs.h) — bool 兼容修复
- [common.mk](common.mk) — 添加 -std=gnu11
- [Makefile](Makefile) — 添加 -Os 优化
- [CHANGE.md](CHANGE.md) — 变更清单
- [TODO.md](TODO.md) — 待办事项清单
