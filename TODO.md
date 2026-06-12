# LAB-9 TODO List

按照 readme.md 的实验步骤和 xv6-labs-2020 的代码思路，以下是按优先级排列的待办事项。

## 第1步：准备工作

### 1.1 [pmem.c] 实现 pmem_stat
- 函数签名已声明：`void pmem_stat(uint32 *free_pages_in_kernel, uint32 *free_pages_in_user);`
- 读取 `kern_region.allocable` 和 `user_region.allocable` 并输出到指针参数
- 参考：xv6-labs-2020 的 `kalloc.c` 中的 freelist 计数方式

### 1.2 [uvm.c] 修改 uvm_heap_grow 支持 flag 参数
- 当前声明：`uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len, int flag);`
- 当前实现缺少 `flag` 参数，需要将 `PTE_R | PTE_W | PTE_U` 替换为 `PTE_U | flag`
- 同时需要修改 `sys_brk` 中的调用，加上默认 flag 参数
- 参考：xv6-labs-2020 kernel/vm.c 的 `uvmalloc`

## 第2步：完善文件系统 (fs)

### 2.1 [dentry.c] 补全目录和路径函数

#### dentry_search_2
- `uint32 dentry_search_2(inode_t *ip, uint32 inode_num, char *name);`
- 类似 `dentry_search`，但同时匹配 `inode_num` 和 `name`
- 找到返回偏移量，否则返回 INVALID_INODE_NUM

#### dentry_transmit  
- `uint32 dentry_transmit(inode_t *ip, uint64 dst, uint32 len, bool is_user_dst);`
- 类似 `dentry_print` 但将有效目录项拷贝到 dst（用户态或内核态）
- 返回传输的字节数

#### inode_to_path
- `uint32 inode_to_path(inode_t *ip, char *path, uint32 len);`
- 逆向解析：从 inode 回溯到根，利用 `..` 目录项
- 逆向填充 path 缓冲区，返回偏移量（path + offset 才是绝对路径起点）

#### path_create_inode
- `inode_t *path_create_inode(char *path, uint16 type, uint16 major, uint16 minor);`
- 调用 `path_to_parent_inode` 获取父目录和文件名
- 调用 `inode_create` + `dentry_create`
- 对于目录类型，创建 `.` 和 `..` 目录项

#### path_link
- `uint32 path_link(char *old_path, char *new_path);`
- old_path 指向的 inode 不能是目录类型
- nlink++ + dentry_create(new_path)
- 参考：xv6-labs-2020 kernel/sysfile.c 的 `sys_link`

#### path_unlink
- `uint32 path_unlink(char *path);`
- dentry_delete + nlink--
- 资源释放由 inode_put 自动处理
- 参考：xv6-labs-2020 kernel/sysfile.c 的 `sys_unlink`

### 2.2 [fs.c] 实现文件操作函数

#### file_init
- `void file_init();`
- 初始化 `lk_file_table` 锁，将 `file_table` 清零

#### file_alloc
- `file_t *file_alloc();`
- 遍历 `file_table`，找到 `ref == 0` 的槽位，ref 设为 1

#### file_open
- `file_t *file_open(char *path, uint32 open_mode);`
- 解析路径，若 OPEN_CREATE 且文件不存在则创建
- 检查设备文件权限（调用 device_open_check）
- 设置 readable/writable/offset
- 参考：xv6-labs-2020 kernel/sysfile.c 的 `sys_open`

#### file_close
- `void file_close(file_t *file);`
- ref--，若 ref == 0 则 inode_put
- 参考：xv6-labs-2020 kernel/file.c 的 `fileclose`

#### file_read
- `uint32 file_read(file_t *file, uint32 len, uint64 dst, bool is_user_dst);`
- switch-case 按 ip->type 分类处理：
  - INODE_TYPE_DATA: 调用 inode_read_data
  - INODE_TYPE_DIR: 调用 dentry_transmit（只读目录项）
  - INODE_TYPE_DIVICE: 调用 device_read_data
- 更新 offset

#### file_write
- `uint32 file_write(file_t *file, uint32 len, uint64 src, bool is_user_src);`
- switch-case 分类处理：
  - INODE_TYPE_DATA: 调用 inode_write_data
  - INODE_TYPE_DIVICE: 调用 device_write_data
- 目录文件不支持写入

#### file_lseek
- `uint32 file_lseek(file_t *file, uint32 lseek_offset, uint32 lseek_flag);`
- LSEEK_SET / LSEEK_ADD / LSEEK_SUB
- 尽力而为移动，不超出文件大小边界

#### file_dup
- `file_t *file_dup(file_t *file);`
- ref++ with lk_file_table protection

#### file_get_stat
- `uint32 file_get_stat(file_t *file, uint64 user_dst);`
- 填充 file_stat_t 并拷贝到用户空间

### 2.3 [device.c] 实现设备文件操作

#### device_init
- `void device_init();`
- 注册 6 个设备：stdin, stdout, stderr, zero, null, gpt0
- 通过 path_create_inode 在 /dev 下创建设备文件 inode
- 在 fs_init 中调用（与 file_init 一起）

#### device_open_check
- `bool device_open_check(uint16 major, uint32 open_mode);`
- 检查 major 是否在有效范围
- 检查 open_mode 与设备读写权限是否匹配

#### device_read_data
- `uint32 device_read_data(uint16 major, uint32 len, uint64 dst, bool is_user_dst);`
- 根据 major 分发到对应设备的 read 函数

#### device_write_data
- `uint32 device_write_data(uint16 major, uint32 len, uint64 src, bool is_user_src);`
- 根据 major 分发到对应设备的 write 函数

## 第3步：进程与文件系统

### 3.1 [proc.c] 进程文件生命周期

#### proc_init
- 初始化 `open_file[]` 数组为 NULL
- 初始化 `cwd` 为 NULL

#### proc_return  
- 对于 proczero：依次打开 stdin(0), stdout(1), stderr(2)
- 设置 cwd = inode_get(ROOT_INODE) -> inode_dup

#### proc_fork
- 复制父进程的 open_file（file_dup 每个非 NULL 条目）
- 复制 cwd（inode_dup）

#### proc_free
- 关闭所有 open_file（file_close 每个非 NULL 条目）
- 释放 cwd（inode_put）
- 设置所有条目为 NULL

### 3.2 [dentry.c] 支持相对路径
- 修改 `__path_to_inode`：如果 path 不以 '/' 开头，则从 `myproc()->cwd` 开始解析
- 处理 `.` 和 `..` 特殊目录项

## 第4步：执行ELF文件

### 4.1 [exec.c] proc_exec
- `int proc_exec(char *path, char **argv);`
- step-0: 准备全新 pagetable 和 trapframe
- step-1: 解析文件路径获取 ELF inode
- step-2: 读取 ELF header，验证 magic
- step-3: 调用 prepare_heap 载入 segments
- step-4: 释放 ELF inode
- step-5: 调用 prepare_stack 处理 argv
- step-6: 释放旧资源（页表、mmap）
- step-7: 设置 trapframe：a0=argc, a1=argv(sp), epc=entry, sp
- step-8: 更新进程字段

## 第5步：系统调用

### 5.1 [syscall.c] 跳转表
- 添加 SYS_exec 到 SYS_unlink 的跳转表项

### 5.2 [sysfunc.c] 系统调用实现

#### sys_exec
- 读取 path 和 argv 参数
- 调用 proc_exec

#### sys_open
- 读取 path 和 open_mode
- 调用 file_open + alloc_fd

#### sys_close
- 读取 fd，从 open_file 取文件
- 调用 file_close，清空 open_file[fd]

#### sys_read
- 读取 fd, len, addr
- 调用 file_read

#### sys_write
- 读取 fd, len, addr
- 调用 file_write

#### sys_lseek
- 读取 fd, offset, flag
- 调用 file_lseek

#### sys_dup
- 读取 fd
- 调用 file_dup + alloc_fd

#### sys_fstat
- 读取 fd, addr
- 调用 file_get_stat

#### sys_get_dentries
- 读取 fd, addr, buf_len
- 调用 file_read（目录文件读取目录项）

#### sys_mkdir
- 读取 path
- 调用 path_create_inode

#### sys_chdir
- 读取 new_path
- 替换 myproc()->cwd

#### sys_print_cwd
- 调用 inode_to_path 获取 cwd 的绝对路径
- 打印并返回

#### sys_link
- 读取 old_path, new_path
- 调用 path_link

#### sys_unlink
- 读取 path
- 调用 path_unlink
