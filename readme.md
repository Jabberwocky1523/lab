# LAB-5: 系统调用流程建立 + 用户态虚拟内存管理

**前言**

在lab-4中, 我们初步实现了第一个用户进程`proczero`

它通过`sys_helloworld`系统调用, 利用内核的系统服务发出了"第一声啼哭"

本次实验的核心目的是完善和发展`proczero`, 具体包括两个方面:

- 赋予`proczero`更强的内存掌控能力, 包括堆、栈、离散映射三个部分

- 赋予`proczero`完善的请求服务能力, 建立真正的系统调用流程

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

**标记说明**

**NEW**: 新增源文件, 直接拷贝即可, 无需修改

**CHANGE**: 旧的源文件发生了更新, 直接拷贝即可, 无需修改

**TODO**: 你需要实现新功能 / 你需要完善旧功能

## 任务1：用户态和内核态的数据迁移

回忆一下上个实验的`sys_helloworld`系统调用, 它的作用是让内核输出`"hello world"`

一个明显的问题: 用户态程序无法向内核程序传递参数, 导致系统服务非常僵硬和受限

我们可以从普通函数的参数传递获得启示, 传参方法无非两种:

- 直接传递值: `add(int a, int b)`, 本质是将参数值放到寄存器里

- 基于地址做间接传递: `strcmp(char *s1, char *s2, int len)`, 本质是将地址放到寄存器里

通过阅读`user/syscall_arch.h`, 可以发现系统调用编号默认放在a7寄存器, a0到a5寄存器则是存放参数

```c
static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    __asm_syscall("r"(a7), "0"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5))
}
```

内核可以通过访问`proc->tf->ax`直接拿到这些参数 (trapframe实在太好用了~)

- 对于值传递, `arg_uint32`和`arg_uint64`可以很好地完成任务

- 对于地址传递, 必须考虑用户地址空间和内核地址空间不匹配的问题:

**用户传入的地址空间是基于用户页表的, 但是进入内核后使用的是内核页表**

解决这个问题需要手动查询用户页表, 找到虚拟地址对应的物理地址, 之后再做数据迁移

请你完成`kernel/mem/uvm.c`的第一部分, 包括`uvm_copyin`、`uvm_copyout`、`uvm_copyin_str`三个部分

随后, 你需要补全`trap_user_handler`中的系统调用的处理逻辑:

- 调用`syscall`进行分类跳转

- 补全三个具体的处理逻辑 `sys_copyin`、`sys_copyout`、`sys_copyinstr`

- 注意: 这三个系统调用只服务于本次测试, 不是长期保存的系统调用

## 测试1：用户态和内核态的数据迁移

测试逻辑: 

- 用户读取内核中的数组 (1 2 3 4 5)

- 用户将读到的数组传递给内核, 内核收到后打印出来

- 用户将自己的字符串传递给内核, 内核收到后打印出来

```c
// in initcode.c
#include "sys.h"

int main()
{
    int L[5];
    char* s = "hello, world"; 
    syscall(SYS_copyout, L);
    syscall(SYS_copyin, L, 5);
    syscall(SYS_copyinstr, s);
    while(1);
    return 0;
}
```

测试结果见`picture/test-1.png`

## 任务2：堆的手动管理与栈的自动管理

上次实验中, 栈空间被设置为4KB, 堆空间被设置为0KB, 对于非常简单的`initcode.c`是足够的

然而, 现实世界的应用程序需要可以动态增长的栈和堆, 本次实验我们做一个初步的实现

### 堆的管理是手动的

**堆-HEAP**为用户提供了一块连续的大范围内存空间, 它的生长方向的是低地址到高地址

内核给用户程序提供了一个`sys_brk`系统调用, 允许用户改变堆顶的位置

`sys_brk`的效果可以进一步分为:

- 空间增加: old_heap_top < new_heap_top 

- 空间减少: old_heap_top > new_heap_top

- 空间不变: old_heap_top == new_heap_top

- 查询当前栈顶: new_heap_top == 0

涉及内存页面的申请释放、用户页表的修改、`proc->heap_top`的更新

请你完成`sys_brk`、`uvm_heap_grow`、`uvm_heap_ungrow`几个函数

### 栈的管理是自动的

**栈-STACK**为用户的临时变量和函数执行提供了一块连续的内存空间, 它的生长方向是高地址到低地址

用户程序无需显式地管理栈空间, 由内核根据用户需要进行自动管理 (自动的内存申请和映射)

内核不会在进程初始化时直接分配一个很大的栈空间 (默认分配4KB), 而是根据程序运行的需要逐步分配足够大的空间

当用户读或写一块未分配的地址空间时, 会触发**13号异常(Load Page Fault)** / **15号异常(Store/AMO Page Fault)**

我们在`trap_user_handler`里识别这两种异常, 然后调用`uvm_ustack_grow`来处理缺页异常

`uvm_ustack_grow`首先判断发生page fault的地址 (放在stval寄存器) 是否是合理的栈扩展地址

确认合法性后: 申请物理页面、修改用户页表、更新`proc->ustack_npage`

需要提醒的是: 一次可以扩展多个页面, 扩展后不会发生收缩 (和堆的管理不同)

### 边界检查

需要提醒的是: 我们在栈和堆的中间区域里, 划分了一段地址空间作为离散内存空间的区域 (mmap_region)

这块区域的起点地址被定义为`MMAP_BEGIN`, 终点被定义为`MMAP_END` (in `kernl/mem/type.h`)

因此, 栈的生长不应该越过`MMAP_END`, 堆的生长不应该越过`MMAP_BEGIN`

mmap_region的详细介绍放在任务3和和任务4, 这里只需要注意边界检查即可

## 测试2：堆的手动管理与栈的自动管理

**堆的管理**

```c
// in initcode.c
#include "sys.h"

#define PGSIZE 4096

int main()
{
    long long heap_top = 0;
    
    heap_top = syscall(SYS_brk, 0);
    heap_top = syscall(SYS_brk, heap_top + PGSIZE * 9);
    heap_top = syscall(SYS_brk, heap_top);
    heap_top = syscall(SYS_brk, heap_top - PGSIZE * 5);

    while(1);
    return 0;
}
```

你需要在`sys_brk`中增加一些调试性输出

测试结果见`picture/test-2(1)(2).png`

**栈的管理**

函数内定义非static的长数组就能让栈的大小超过4KB

你也可以通过深度函数递归来实现类似的效果 (比如汉诺塔问题)

```c
// in initcode.c
#include "sys.h"

#define PGSIZE 4096

int main()
{
    char tmp[PGSIZE * 4];

    tmp[PGSIZE * 3] = 'h';
    tmp[PGSIZE * 3 + 1] = 'e';
    tmp[PGSIZE * 3 + 2] = 'l';
    tmp[PGSIZE * 3 + 3] = 'l';
    tmp[PGSIZE * 3 + 4] = 'o';
    tmp[PGSIZE * 3 + 5] = '\0';

    syscall(SYS_copyinstr, tmp + PGSIZE * 3);

    tmp[0] = 'w';
    tmp[1] = 'o';
    tmp[2] = 'r';
    tmp[3] = 'l';
    tmp[4] = 'd';
    tmp[5] = '\0';

    syscall(SYS_copyinstr, tmp);

    while (1);
    return 0;
}
```

你需要在`trap_user_handler`中增加一些调试性输出

测试结果见`picture/test-3.png`

## 任务3: mmap_region_node 仓库管理

应用程序有了堆和栈就足够了吗? 应用程序有时需要临时申请一块内存空间, 过一会就释放掉

- 用栈来申请的话无法手动释放 (释放函数里数组占用的空间?)

- 用堆来申请的话可能面临碎片化风险 (堆更适合管理大片逻辑连续的内存空间)

因此, 我们需要设计一种可以动态申请释放的离散内存资源管理方法

直观的想法就是链表结构: 将多个内存资源节点通过链表链接在一起, 在进程结构体里存储表头!

说明: 在真实的操作系统里, 堆、栈、内存映射区的细节和定位与我们这里说的有所区别

结构体 `mmap_region_t` 用于描述一块连续地址空间, 它起始于`begin`, 包括`npages`个页面

进程会记录地址空间中的第一个`mmap_region_t`, 各个资源节点通过`next`指针串联 (构成单链表)

**特别提醒: mma_region_t 描述的是已分配出去的空间, 和2024版本是反过来的!**

```c
/* mmap_region 描述了一个 mmap区域 */
typedef struct mmap_region
{
    uint64 begin;             // 起始地址
    uint32 npages;            // 管理的页面数量
    struct mmap_region *next; // 链表指针
} mmap_region_t;
```

理解这部分后我们继续考虑另一个问题: `mmap_region_t`结构体本身也是一种资源

我们规定OS内核可以提供`N_MMAP`个这样的结构体, 各个进程需要有序获取该资源

为了保证各个进程可以高效和有序地共享这种资源, 我们在`kernel/mem/mmap.c`里维护了一个资源仓库

```c
/* mmap_region_node 是 mmap_region 在仓库里的包装 */
typedef struct mmap_region_node
{
    mmap_region_t mmap;
    struct mmap_region_node *next;
} mmap_region_node_t;


// mmap_region_node_t 仓库(单向链表) + 链表头节点(不可分配) + 保护仓库的自旋锁
static mmap_region_node_t node_list[N_MMAP];
static mmap_region_node_t list_head;
static spinlock_t list_lk;
```

具体来说:

- 首先将 `mmap_region_t` 包装为 `mmap_region_node_t`, 以维护资源仓库的单链表结构

- 然后通过全局的自旋锁 `list_lk` 确保任何时候只有一个进程在获取资源或释放资源

- 提供`mmap_init`、`mmap_region_alloc`、`mmap_region_free`作为资源仓库的对外接口

## 测试3: mmap_region_node 仓库管理

我们先来测试一下, 作为资源仓库, 它能不能在多核竞争的条件下保证资源申请和释放的有序性

```c
// in main.c
volatile static int started = 0;
volatile static bool over_1 = false, over_2 = false;
volatile static bool over_3 = false, over_4 = false;

void* mmap_list[N_MMAP];

int main()
{
    int cpuid = r_tp();

    if(cpuid == 0) {
        
        print_init();
        printf("cpu %d is booting!\n", cpuid);
        pmem_init();
        kvm_init();
        kvm_inithart();
        trap_kernel_init();
        trap_kernel_inithart();
        
        // 初始化 + 初始状态显示
        mmap_init();
        mmap_show_nodelist();
        printf("\n");

        __sync_synchronize();
        started = 1;

        // 申请
        for(int i = 0; i < N_MMAP / 2; i++)
            mmap_list[i] = mmap_region_alloc();
        over_1 = true;

        // 屏障
        while(over_1 == false ||  over_2 == false);

        // 释放
        for(int i = 0; i < N_MMAP / 2; i++)
            mmap_region_free(mmap_list[i]);
        over_3 = true;

        // 屏障
        while (over_3 == false || over_4 == false);

        // 查看结束时的状态
        mmap_show_nodelist();        

    } else {

        while(started == 0);
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
        kvm_inithart();
        trap_kernel_inithart();

        // 申请
        for(int i = N_MMAP / 2; i < N_MMAP; i++)
            mmap_list[i] = mmap_region_alloc();
        over_2 = true;

        // 屏障
        while(over_1 == false || over_2 == false);

        // 释放
        for(int i = N_MMAP / 2; i < N_MMAP; i++)
            mmap_region_free(mmap_list[i]);
        over_4 = true;
    }

    while (1);
}
```

测试结果见`picture/test-4(1)(2).png`

- 第一部分的输出应该是 `node X index = X` (X从0增加到255)

- 第二部分输出应该是两股输出交替 (node从0增加到255, 一股index从255减到128, 另一股index从127减到0)

## 任务4: mmap 与 munmap

资源仓库的建立使得 `mmap_region_t` 结构体的申请和释放更加方便和安全, 服务于mmap和munamp操作

我们以mmap为例, 从系统调用出发, 梳理它的逻辑过程:

- 用户程序发出 `sys_mmap(uint64 begin, uint32 len)` 申请一块内存空间

- 调用`uvm_mmap(begin, len / PGSIZE, PTE_R | PTE_W)`进行具体处理

- `uvm_mmap()`首先创建一个新的 mmap_region_t 用于描述这块新的地址空间

- 随后将将这块 new_mmap_region 插入进程 mmap 链表的合适位置 (保持整体有序)

- 新插入的节点可能和前面的节点相邻, 可能和后面的节点相邻, 也可能同时相邻

- 考虑到仓库里资源受限的问题, 我们应该将相邻的节点进行尽可能的合并 (逻辑较为复杂, 建议你画图分析)

- 我们提供了辅助函数`mmap_merge()`用于帮助你完成这些合并, 你可以研究一下怎么用

- 合并完成后, 进行物理页申请和页表修改的步骤 (这里比较简单)

**注意: 当用户传入的begin=0时, 从头到尾扫描, 找到第一个足够大的空间即可**

**另外: 记得为proc结构体增加mmap字段, 并在proc_make_first函数中增加对应的初始化逻辑**

munmap的整体流程于mmap相近, 你应该具备举一反三的能力, 这里不做详细介绍

## 测试4: mmap 与 munmap

我们给出了测试用例用于检测uvm_mmap()和uvm_munmap()中可能的遗漏和错误

请你理解它在测试哪些情况, 以及预期的输出是什么样的

当然, 你应该补充更多测试用例, 以确保实现的完备性

```c
// in initcode.c
#include "sys.h"

// 与内核保持一致
#define VA_MAX       (1ul << 38)
#define PGSIZE       4096
#define MMAP_END     (VA_MAX - (16 * 256 + 2) * PGSIZE)
#define MMAP_BEGIN   (MMAP_END - 64 * 256 * PGSIZE)

int main()
{
    // 建议画图理解这些地址和长度的含义

    // sys_mmap 测试 
    syscall(SYS_mmap, MMAP_BEGIN + 4 * PGSIZE, 3 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 10 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 2 * PGSIZE,  2 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 12 * PGSIZE, 1 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 7 * PGSIZE, 3 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN, 2 * PGSIZE);
    syscall(SYS_mmap, 0, 10 * PGSIZE);

    // sys_munmap 测试
    syscall(SYS_munmap, MMAP_BEGIN + 10 * PGSIZE, 5 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN, 10 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 17 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 15 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 19 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 22 * PGSIZE, 1 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 21 * PGSIZE, 1 * PGSIZE);

    while(1);
    return 0;
}
```

请你在`sys_mmap()`和`sys_munmap()`中增加提示性输出

```c
    proc_t *p = myproc();
    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");
```

测试结果见`picture/test-5(1)(2)(3)(4)(5)(6)(7)(8).png`

## 任务5: 页表的复制与销毁

虽然目前我们只有一个进程且永不退出，但是需要为下一个实验做一些准备

你需要完成页表复制和销毁的函数 uvm_destroy_pgtbl() 和 uvm_copy_pgtbl()

需要提醒的是:

- 第一个函数考虑如何使用递归完成

- 第二个函数深入理解用户地址空间各个区域的特点

## 测试5: 页表的复制与销毁

请你参考前4个测试点的设计, 自行决定如何测试页表的复制和销毁

**尾声**

本次实验大概分成以下三个逻辑阶段:

- 首先关注如何实现用户态和内核态的数据传递 (以trapframe为媒介), 并建立规范的系统调用流程

- 随后讨论了用户态内存空间的管理: 堆、栈、mmap_region

- 最后讨论了用户页表整体的复制和销毁, 为下一个实验做准备

经过两次实验的打磨, proczero现在已经比较强大和完善了, 但是似乎有些孤单?

**我们将在下一个实验引入它的子子孙孙, 从单进程走向多进程！**

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

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `src/kernel/mem/uvm.c` | TODO → 完整实现 | 用户态虚拟内存管理核心 |
| `src/kernel/mem/mmap.c` | TODO → 完整实现 | mmap 资源仓库 |
| `src/kernel/syscall/sysfunc.c` | TODO → 完整实现 | 6 个系统调用处理函数 |
| `src/kernel/trap/trap_user.c` | TODO → 更新 | 系统调用分发 + 缺页处理 |
| `src/kernel/proc/proc.c` | TODO → 更新 | mmap 字段初始化 |
| `src/kernel/main.c` | CHANGE → 更新 | 添加 mmap_init() 调用 |
| `src/kernel/mem/method.h` | CHANGE → 更新 | 添加 check_inkernel 声明，修改 uvm_mmap 返回类型 |