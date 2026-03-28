#include <stdarg.h>

#include "riscv.h"
#include "spinlock.h"
#include "def.h"

volatile int panicking = 0; // printing a panic message
volatile int panicked = 0;  // spinning forever at end of a panic

// lock to avoid interleaving concurrent printf's.
static struct
{
  struct spinlock lock;
} pr;

static char digits[] = "0123456789abcdef";
// 0x100:空格
void consputc(int c)
{
  if (c == 0x100)
  {
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b');
    uartputc_sync(' ');
    uartputc_sync('\b');
  }
  else
  {
    uartputc_sync(c);
  }
}

static void printint(long long xx, int base, int sign)
{
  char buf[20];
  int i;
  unsigned long long x;

  if (sign && (sign = (xx < 0)))
    x = -xx;
  else
    x = xx;

  i = 0;
  do
  {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);

  if (sign)
    buf[i++] = '-';

  while (--i >= 0)
    consputc(buf[i]);
}

static void printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

static void printfloat(double f, int precision)
{
  int i = 0;
  long long int_part;
  double frac_part;
  printint(10, 10, 0);
  if (f < 0)
  {
    consputc('-');
    f = -f;
  }

  if (f != f)
  {
    for (const char *s = "nan"; *s; s++)
      consputc(*s);
    return;
  }
  if (f > 1.7976931348623157e+308)
  {
    for (const char *s = "inf"; *s; s++)
      consputc(*s);
    return;
  }

  int_part = (long long)f;
  frac_part = f - int_part;

  if (int_part == 0)
  {
    consputc('0');
  }
  else
  {
    char int_buf[30];
    int int_idx = 0;
    while (int_part > 0)
    {
      int_buf[int_idx++] = digits[int_part % 10];
      int_part /= 10;
    }
    while (--int_idx >= 0)
      consputc(int_buf[int_idx]);
  }

  if (precision > 0)
  {
    consputc('.');
    for (i = 0; i < precision; i++)
    {
      frac_part *= 10;
      int digit = (int)frac_part;
      consputc(digits[digit]);
      frac_part -= digit;
    }
  }
}

// Print to the console.
int printf(char *fmt, ...)
{
  va_list ap;
  int i, cx;
  char *s;
  int length_mod = 0; // 0: none, 1: l, 2: ll
  int base, sign;

  if (panicking == 0)
    acquire(&pr.lock);

  va_start(ap, fmt);
  for (i = 0; (cx = fmt[i] & 0xff) != 0; i++)
  {
    if (cx != '%')
    {
      consputc(cx);
      continue;
    }
    i++;
    length_mod = 0;

    if (fmt[i] == 'l')
    {
      length_mod = 1;
      i++;
      if (fmt[i] == 'l')
      {
        length_mod = 2;
        i++;
      }
    }

    cx = fmt[i] & 0xff;

    // Handle format specifiers
    switch (cx)
    {
    case 'd':
      base = 10;
      sign = 1;
      if (length_mod == 0)
        printint(va_arg(ap, int), base, sign);
      else
        printint(va_arg(ap, uint64), base, sign);
      break;

    case 'u':
      base = 10;
      sign = 0;
      if (length_mod == 0)
        printint(va_arg(ap, uint32), base, sign);
      else
        printint(va_arg(ap, uint64), base, sign);
      break;

    case 'x':
      base = 16;
      sign = 0;
      if (length_mod == 0)
        printint(va_arg(ap, uint32), base, sign);
      else
        printint(va_arg(ap, uint64), base, sign);
      break;

    case 'X':
      base = 16;
      sign = 0;
      if (length_mod == 0)
        printint(va_arg(ap, uint32), base, sign);
      else
        printint(va_arg(ap, uint64), base, sign);
      break;

    case 'o':
      base = 8;
      sign = 0;
      if (length_mod == 0)
        printint(va_arg(ap, uint32), base, sign);
      else
        printint(va_arg(ap, uint64), base, sign);
      break;

    case 'p':
      printptr(va_arg(ap, uint64));
      break;

    case 'c':
      consputc(va_arg(ap, uint));
      break;

    case 's':
      if ((s = va_arg(ap, char *)) == 0)
        s = "(null)";
      for (; *s; s++)
        consputc(*s);
      break;

    case 'f':
      printfloat(va_arg(ap, double), 6);
      break;

    case '%':
      consputc('%');
      break;

    default:
      consputc('%');
      consputc(cx);
      break;
    }
  }
  va_end(ap);

  if (panicking == 0)
    release(&pr.lock);

  return 0;
}

void panic(char *s)
{
  panicking = 1;
  printf("panic: ");
  printf("%s\n", s);
  panicked = 1; // freeze uart output from other CPUs
  for (;;)
    ;
}

void printfinit(void)
{
  initlock(&pr.lock, "pr");
}
