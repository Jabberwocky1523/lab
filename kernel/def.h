#ifndef DEF_H
struct spinlock;

// printf.c
int printf(char *fmt, ...);
void printfinit();
void panic(char *s);

// spinlock.c
void acquire(struct spinlock *);
int holding(struct spinlock *);
void initlock(struct spinlock *, char *);
void release(struct spinlock *);
void push_off(void);
void pop_off(void);

// uart.c
void uartputc_sync(int);

// proc.c
int cpuid();

// cpu
#define NCPU 8

// type
#define uint unsigned int
#define ushort unsigned short
#define uchar unsigned char
#define uint8 unsigned char
#define uint16 unsigned short
#define uint32 unsigned int
#define uint64 unsigned long

// mem
#define UART0 0x10000000L
#define UART0_IRQ 10
#define TRAMPOLINE (MAXVA - PGSIZE)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)

#endif
