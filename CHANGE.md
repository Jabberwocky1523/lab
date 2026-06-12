# LAB-9 Change Log

## 文件变更清单

### 需要实现的文件（TODO 函数体为空，需要填充）

| 文件 | 状态 | 说明 |
|------|------|------|
| [src/kernel/mem/pmem.c](src/kernel/mem/pmem.c) | TODO | 实现 `pmem_stat` |
| [src/kernel/mem/uvm.c](src/kernel/mem/uvm.c) | TODO | 修改 `uvm_heap_grow` 签名增加 flag 参数 |
| [src/kernel/fs/dentry.c](src/kernel/fs/dentry.c) | TODO | 实现 6 个函数：dentry_search_2, dentry_transmit, inode_to_path, path_create_inode, path_link, path_unlink |
| [src/kernel/fs/device.c](src/kernel/fs/device.c) | TODO | 实现 4 个函数：device_init, device_open_check, device_read_data, device_write_data |
| [src/kernel/fs/fs.c](src/kernel/fs/fs.c) | TODO | 实现 9 个函数：file_init, file_alloc, file_open, file_close, file_read, file_write, file_lseek, file_dup, file_get_stat |
| [src/kernel/proc/proc.c](src/kernel/proc/proc.c) | TODO | 修改 proc_init/proc_return/proc_fork/proc_free 支持 open_file 和 cwd |
| [src/kernel/proc/exec.c](src/kernel/proc/exec.c) | TODO | 实现 `proc_exec` |
| [src/kernel/syscall/syscall.c](src/kernel/syscall/syscall.c) | TODO | 补充跳转表（SYS_exec ~ SYS_unlink） |
| [src/kernel/syscall/sysfunc.c](src/kernel/syscall/sysfunc.c) | TODO | 实现 14 个系统调用函数 |

### 需要小改的文件

| 文件 | 状态 | 说明 |
|------|------|------|
| [src/kernel/fs/fs.c](src/kernel/fs/fs.c) | CHANGE | fs_init 中需要调用 device_init 和 file_init（目前死循环 while(1)） |
| [src/kernel/syscall/sysfunc.c](src/kernel/syscall/sysfunc.c) | CHANGE | sys_brk 调用 uvm_heap_grow 时需要增加 flag 参数 |
| [src/kernel/lib/print.c](src/kernel/lib/print.c) | CHANGE | print_init 中调用 console_init（readme 提示） |
| [src/kernel/lib/uart.c](src/kernel/lib/uart.c) | CHANGE | uart_intr 中 switch-case 改为 cons_edit |

### 不需要修改的文件（已由助教提供或标记为 NEW/CHANGE）

| 文件 | 状态 |
|------|------|
| [src/loader/kernel.ld](src/loader/kernel.ld) | NEW/CHANGE |
| [src/loader/user.ld](src/loader/user.ld) | NEW |
| [src/kernel/lib/console.c](src/kernel/lib/console.c) | NEW |
| [src/user/help.c](src/user/help.c) | NEW |
| [src/user/help.h](src/user/help.h) | NEW |
| [src/user/syscall.c](src/user/syscall.c) | NEW |
| [src/user/syscall_num.h](src/user/syscall_num.h) | CHANGE |
| [src/user/test_1.c](src/user/test_1.c) | NEW |
| [src/user/test_2.c](src/user/test_2.c) | NEW |
| [src/user/test_3.c](src/user/test_3.c) | NEW |
| [src/user/test_4.c](src/user/test_4.c) | NEW |
| [src/kernel/fs/type.h](src/kernel/fs/type.h) | CHANGE (含 file_t, device_t, file_stat_t 等) |
| [src/kernel/fs/method.h](src/kernel/fs/method.h) | CHANGE |
| [src/kernel/lib/method.h](src/kernel/lib/method.h) | CHANGE |
| [src/kernel/lib/type.h](src/kernel/lib/type.h) | CHANGE |
| [src/kernel/mem/method.h](src/kernel/mem/method.h) | CHANGE |
| [src/kernel/proc/type.h](src/kernel/proc/type.h) | CHANGE |
| [src/kernel/proc/method.h](src/kernel/proc/method.h) | CHANGE |
| [src/kernel/syscall/type.h](src/kernel/syscall/type.h) | CHANGE |
| [src/kernel/syscall/method.h](src/kernel/syscall/method.h) | CHANGE |
| [src/kernel/fs/mod.h](src/kernel/fs/mod.h) | CHANGE |

## 实现顺序

按照依赖关系排列：

1. **pmem.c** - pmem_stat（最底层，被 device_gpt0_write 使用）
2. **uvm.c** - uvm_heap_grow flag（被 sys_brk 和 exec.c 使用）
3. **fs/dentry.c** - 6 个函数（被 fs.c, sysfunc.c 使用）
4. **fs/device.c** - 4 个函数（被 fs.c 使用）
5. **fs/fs.c** - 9 个文件函数（被 proc.c, sysfunc.c 使用）
6. **proc/proc.c** - open_file/cwd 生命周期（被 sysfunc.c, dentry.c 使用）
7. **proc/exec.c** - proc_exec（最复杂的函数）
8. **syscall/syscall.c** - 跳转表
9. **syscall/sysfunc.c** - 14 个系统调用

## 测试方法

```bash
make build    # 编译
make run      # 运行 QEMU


理想输出见 `picture/` 目录下的截图。
