#ifndef SPINLOCK_H
// 互斥锁。
struct spinlock
{
  int locked; // 锁是否被持有？

  // 用于调试：
  char *name;      // 锁的名称。
  struct cpu *cpu; // 持有锁的 cpu。
};
#endif
