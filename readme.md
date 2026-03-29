# lab-3 中断异常初步
## 0.代码组织结构
```
ECNU-OSLAB-2025-TASK
├── LICENSE        开源协议
├── .vscode        配置了可视化调试环境
├── registers.xml  配置了可视化调试环境
├── common.mk      Makefile中一些工具链的定义
├── Makefile       编译运行整个项目
├── kernel.ld      定义了内核程序在链接时的布局
├── pictures       README使用的图片目录 (CHANGE, 日常更新)
├── README.md      实验指导书 (CHANGE, 日常更新)
└── src            源码
    └── kernel     内核源码
        ├── arch   RISC-V相关
        │   ├── method.h
        │   ├── mod.h
        │   └── type.h (CHANGE, 新增一些RISC-V中断相关宏定义)
        ├── boot   机器启动
        │   ├── entry.S
        │   └── start.c (TODO, 在M-mode多做一些事情再进入S-mode)
        ├── lock   锁机制
        │   ├── spinlock.c
        │   ├── method.h
        │   ├── mod.h
        │   └── type.h
        ├── lib    常用库
        │   ├── cpu.c
        │   ├── print.c
        │   ├── uart.c
        │   ├── utils.c
        │   ├── method.h
        │   ├── mod.h
        │   └── type.h
        ├── mem    内存模块
        │   ├── pmem.c
        │   ├── kvm.c
        │   ├── method.h
        │   ├── mod.h
        │   └── type.h
        ├── trap   陷阱模块
        │   ├── plic.c (NEW, 请阅读和理解这部分)
        │   ├── timer.c (TODO, 时钟中断和计时器相关操作)
        │   ├── trap_kernel.c (TODO, 内核态trap处理的核心逻辑)
        │   ├── trap.S (NEW, 很重要, 请完全理解这部分)
        │   ├── method.h (CHANGE)
        │   ├── mod.h
        │   └── type.h (CHANGE)
        └── main.c (TODO, 更多的初始化)
```

