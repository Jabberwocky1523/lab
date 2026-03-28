
#include "riscv.h"
#include "def.h"
void main();
void timerinit();

// entry.S 每个 CPU 需要一个栈。
__attribute__((aligned(16))) char stack0[4096 * NCPU];

// entry.S 在机器模式下跳转到这里的 stack0。
void start()
{
  // 将 M 之前的特权模式设置为监督者模式，用于 mret。
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);

  // 将 M 异常程序计数器设置为 main，用于 mret。
  // 需要 gcc -mcmodel=medany
  w_mepc((uint64)main);

  // 暂时禁用分页。
  w_satp(0);

  // 将所有中断和异常委托给监督者模式。
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE);

  // 配置物理内存保护，使监督者模式
  // 能够访问所有物理内存。
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // 请求时钟中断。
  timerinit();

  // 将每个 CPU 的 hartid 保留在其 tp 寄存器中，用于 cpuid()。
  int id = r_mhartid();
  w_tp(id);

  // 切换到监督者模式并跳转到 main()。
  asm volatile("mret");
}

// 请求每个 hart 生成定时器中断。
void timerinit()
{
  // 启用监督者模式定时器中断。
  w_mie(r_mie() | MIE_STIE);

  // 启用 sstc 扩展（即 stimecmp）。
  w_menvcfg(r_menvcfg() | (1L << 63));

  // 允许监督者使用 stimecmp 和 time。
  w_mcounteren(r_mcounteren() | 2);

  // 请求第一次定时器中断。
  w_stimecmp(r_time() + 1000000);
}
