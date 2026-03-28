#include "riscv.h"
#include "def.h"
volatile static int started = 0;

// start() 在所有 CPU 的监督模式下跳转到这里。
int main()
{
  printfinit();
  printf("HelloWorld");
  return 0;
}
