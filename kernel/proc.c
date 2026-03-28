
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "def.h"

struct cpu cpus[NCPU];

int nextpid = 1;

// 必须在禁用中断的情况下调用，
// 以防止进程被移动到不同的 CPU 时发生竞争。
int cpuid()
{
  int id = r_tp();
  return id;
}

// 返回此 CPU 的 cpu 结构体。
// 必须禁用中断。
struct cpu *mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}
