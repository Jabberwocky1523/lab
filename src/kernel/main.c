#include "arch/mod.h"
#include "lib/mod.h"
volatile static int started = 0;
static spinlock_t sum_lk;
volatile static int sum = 0;
void test1()
{
    int cpuid = r_tp();
    if (cpuid == 0)
    {
        print_init();
        printf("Hello world\n");
        started = 1;
    }
    else
    {
        while (started == 0)
            ;
        started = 1;
        printf("Hello world\n", cpuid);
    }
}
void test2()
{
    int cpuid = r_tp();
    if (cpuid == 0)
    {
        spinlock_init(&sum_lk, "sum_lk");
        print_init();
        printf("cpu %d is booting!\n", cpuid);
        __sync_synchronize();
        started = 1;
        int i = 0;
        for (; i < 100000; i++)
        {
            // spinlock_acquire(&sum_lk);
            sum++;
            // spinlock_release(&sum_lk);
        }
        printf("cpu %d report: sum = %d\n", cpuid, sum);
    }
    else
    {
        while (started == 0)
            ;
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
        for (int i = 0; i < 100000; i++)
        {
            spinlock_acquire(&sum_lk);
            sum++;
            spinlock_release(&sum_lk);
        }
        printf("cpu %d report: sum = %d\n", cpuid, sum);
    }
}
int main()
{
    test1();
    return 0;
}
