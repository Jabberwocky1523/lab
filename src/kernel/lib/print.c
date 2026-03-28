/* 标准输出和报错机制 */

#include "mod.h"

static char digits[] = "0123456789abcdef";

/* 如果发生panic, UART的停止标志 */
volatile int panicked = 0;

/* printf的自旋锁 */
static spinlock_t print_lk;

/* 初始化uart + 初始化printf锁 */
void print_init(void)
{
    uart_init();
    spinlock_init(&print_lk, "printf");
}

/* %d %p */
static void printint(int xx, int base, int sign)
{
    char buf[16];
    int i;
    uint32 x;

    if (sign && (sign = xx < 0))
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
        uart_putc_sync(buf[i]);
}

/* %x */
static void printptr(uint64 x)
{
    uart_putc_sync('0');
    uart_putc_sync('x');
    for (int i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        uart_putc_sync(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

/* %f */
static void printfloat(double f, int precision)
{
    int i = 0;
    long long int_part;
    double frac_part;
    printint(10, 10, 0);
    if (f < 0)
    {
        uart_putc_sync('-');
        f = -f;
    }

    if (f != f)
    {
        for (const char *s = "nan"; *s; s++)
            uart_putc_sync(*s);
        return;
    }
    if (f > 1.7976931348623157e+308)
    {
        for (const char *s = "inf"; *s; s++)
            uart_putc_sync(*s);
        return;
    }

    int_part = (long long)f;
    frac_part = f - int_part;

    if (int_part == 0)
    {
        uart_putc_sync('0');
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
            uart_putc_sync(int_buf[int_idx]);
    }

    if (precision > 0)
    {
        uart_putc_sync('.');
        for (i = 0; i < precision; i++)
        {
            frac_part *= 10;
            int digit = (int)frac_part;
            uart_putc_sync(digits[digit]);
            frac_part -= digit;
        }
    }
}

/*
    标准化输出, 需要支持:
    1. %d (32位有符号数,以10进制输出)
    2. %p (32位无符号数,以16进制输出)
    3. %x (64位无符号数,以0x开头的16进制输出)
    4. %c (单个字符)
    5. %s (字符串)
    提示: stdarg.h中的va_list中包括你需要的参数地址
*/
void printf(const char *fmt, ...)
{
    va_list ap;
    int i, cx;
    char *s;
    int length_mod = 0; // 0: none, 1: l, 2: ll
    int base, sign;
    spinlock_acquire(&print_lk);

    va_start(ap, fmt);
    for (i = 0; (cx = fmt[i] & 0xff) != 0; i++)
    {
        if (cx != '%')
        {
            uart_putc_sync(cx);
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

        // 处理格式化说明符
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
            uart_putc_sync(va_arg(ap, int));
            break;

        case 's':
            if ((s = va_arg(ap, char *)) == 0)
                s = "(null)";
            for (; *s; s++)
                uart_putc_sync(*s);
            break;

        case 'f':
            printfloat(va_arg(ap, double), 6);
            break;

        case '%':
            uart_putc_sync('%');
            break;

        default:
            uart_putc_sync('%');
            uart_putc_sync(cx);
            break;
        }
    }
    va_end(ap);
    spinlock_release(&print_lk);
    return;
}

/* 报错并终止输出 */
void panic(const char *s)
{
    printf("panic! %s\n", s);
    panicked = 1;
    while (1)
        ;
}

/* 如果不满足条件, 则调用panic */
void assert(bool condition, const char *warning)
{
    if (!condition)
    {
        panic(warning);
    }
}
