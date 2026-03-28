#ifndef PAOC_H
// 为内核上下文切换保存的寄存器。
struct context
{
  uint64 ra;
  uint64 sp;

  // 被调用者保存的寄存器
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// 每个 CPU 的状态。
struct cpu
{
  struct proc *proc;      // 在此 cpu 上运行的进程，或为 null。
  struct context context; // swtch() 到这里以进入调度器。
  int noff;               // push_off() 嵌套的深度。
  int intena;             // 在 push_off() 之前是否启用了中断？
};

extern struct cpu cpus[8];

struct cpu *mycpu(void);
#endif
