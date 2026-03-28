        # qemu -kernel 将内核加载到 0x80000000
        # 并使每个 hart（即 CPU）跳转到那里。
        # kernel.ld 使以下代码被放置在
        # 0x80000000 处。
.section .text
.global _entry
_entry:
        # 为 C 设置一个栈。
        # stack0 在 start.c 中声明，
        # 每个 CPU 有一个 4096 字节的栈。
        # sp = stack0 + ((hartid + 1) * 4096)
        la sp, stack0
        li a0, 1024*4
        csrr a1, mhartid
        addi a1, a1, 1
        mul a0, a0, a1
        add sp, sp, a0
        # 跳转到 start.c 中的 start()
        call start
spin:
        j spin
