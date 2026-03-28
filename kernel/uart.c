
#include "riscv.h"
#include "def.h"

// UART 控制寄存器是内存映射的，
// 位于地址 UART0。此宏返回
// 其中一个寄存器的地址。
#define Reg(reg) ((volatile unsigned char *)(UART0 + (reg)))

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// UART 控制寄存器。
// 某些寄存器对于读和写有不同的含义。
// 参见 http://byterunner.com/16550.html
#define RHR 0 // 接收保持寄存器（用于输入字节）
#define THR 0 // 发送保持寄存器（用于输出字节）
#define IER 1 // 中断使能寄存器
#define IER_RX_ENABLE (1 << 0)
#define IER_TX_ENABLE (1 << 1)
#define FCR 2 // FIFO 控制寄存器
#define FCR_FIFO_ENABLE (1 << 0)
#define FCR_FIFO_CLEAR (3 << 1) // 清除两个 FIFO 的内容
#define ISR 2                   // 中断状态寄存器
#define LCR 3                   // 线路控制寄存器
#define LCR_EIGHT_BITS (3 << 0)
#define LCR_BAUD_LATCH (1 << 7) // 设置波特率的特殊模式
#define LSR 5                   // 线路状态寄存器
#define LSR_RX_READY (1 << 0)   // 输入正在等待从 RHR 读取
#define LSR_TX_IDLE (1 << 5)    // THR 可以接受另一个要发送的字符

// 向 uart 写入一个字节而不使用
// 中断，供内核 printf() 使用
// 和回显字符。它自旋等待 uart 的
// 输出寄存器为空。
void uartputc_sync(int c)
{
  // 等待 UART 在 LSR 中设置发送保持为空。
  while ((ReadReg(LSR) & LSR_TX_IDLE) == 0)
    ;
  WriteReg(THR, c);
}
