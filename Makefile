# # 引入通用配置文件
# include common.mk

# ifndef TOOLPREFIX
# TOOLPREFIX := $(shell if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
# 	then echo 'riscv64-unknown-elf-'; \
# 	elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
# 	then echo 'riscv64-elf-'; \
# 	elif riscv64-none-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
# 	then echo 'riscv64-none-elf-'; \
# 	elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
# 	then echo 'riscv64-linux-gnu-'; \
# 	elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
# 	then echo 'riscv64-unknown-linux-gnu-'; \
# 	else echo "***" 1>&2; \
# 	echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
# 	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
# 	echo "***" 1>&2; exit 1; fi)
# endif

# # 配置CPU核心数量
# CPUNUM = 2
# # 定义目标文件输出目录
# TARGET = target
# # 定义各模块路径
# KernelPath = src/kernel
# UserPath = src/user
# # 内核链接脚本
# KERNEL_LD  = kernel.ld
# # 定义内核目标文件路径
# ELFKernel = $(TARGET)/kernel/kernel-qemu.elf
# NakedKernel = $(TARGET)/kernel/kernel-qemu.bin

# # 收集内核源代码文件（.c和.S汇编文件）
# KernelSourceFile = $(wildcard $(KernelPath)/*.c) $(wildcard $(KernelPath)/*.S)
# KernelSourceFile += $(wildcard $(KernelPath)/*/*.c) $(wildcard $(KernelPath)/*/*.S)
# # 收集用户态源代码文件
# UserSourceFile = $(wildcard $(UserPath)/*.c)

# # 生成目标文件（.o）路径列表
# KernelOBJ = $(patsubst $(KernelPath)/%.S, $(TARGET)/kernel/%.o, $(filter %.S, $(KernelSourceFile)))
# KernelOBJ += $(patsubst $(KernelPath)/%.c, $(TARGET)/kernel/%.o, $(filter %.c, $(KernelSourceFile)))
# UserOBJ = $(patsubst $(UserPath)/%.c, $(TARGET)/user/%.o, $(filter %.c, $(UserSourceFile)))

# # QEMU模拟器配置
# QEMU     = qemu-system-riscv64  # 指定QEMU程序
# QEMUOPTS = -machine virt -bios none -kernel $(TARGET)/kernel/kernel-qemu.elf  # 基础启动参数
# QEMUOPTS += -m 128M -smp $(CPUNUM) -nographic  # 内存、CPU数量及无图形界面配置

# # 调试相关配置
# GDBPORT = $(shell expr `id -u` % 5000 + 25000)  # 动态计算GDB端口号
# # 根据QEMU版本选择合适的GDB调试参数
# QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
# 	then echo "-gdb tcp::$(GDBPORT)"; \
# 	else echo "-s -p $(GDBPORT)"; fi)

# # 生成GDB初始化文件
# .gdbinit: .gdbinit.tmpl-riscv
# 	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

# # 运行目标：先构建再启动QEMU
# run: build
# 	$(QEMU) $(QEMUOPTS)

# # 调试目标：启动带GDB调试的QEMU
# debug: $(KERN) .gdbinit
# 	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

# # 构建目标：创建输出目录并编译内核
# build: $(TARGET) $(ELFKernel) 

# # 创建输出目录结构（如果不存在）
# .PHONY: $(TARGET)
# $(TARGET):
# ifeq ($(wildcard $(TARGET)),)
# 	@mkdir -p $(TARGET)/kernel
# 	@mkdir -p $(TARGET)/kernel/arch
# 	@mkdir -p $(TARGET)/kernel/boot
# 	@mkdir -p $(TARGET)/kernel/lock
# 	@mkdir -p $(TARGET)/kernel/lib
# endif

# # 编译规则：将汇编文件(.S)编译为目标文件(.o)
# $(TARGET)/kernel/%.o: $(KernelPath)/%.S
# 	$(CC) $(CFLAGS) -c -o $@ $<

# # 编译规则：将C文件(.c)编译为目标文件(.o)
# $(TARGET)/kernel/%.o: $(KernelPath)/%.c
# 	$(CC) $(CFLAGS) -c -o $@ $<

# # 编译用户态程序
# $(TARGET)/user/%.o: $(UserPath)/%.c
# 	$(CC) $(CFLAGS) -c -o $@ $<

# # 链接生成内核ELF文件
# $(ELFKernel): $(KernelOBJ) $(UserOBJ)
# 	$(LD) $(LDFLAGS) -T $(KERNEL_LD) $^ -o $@

# # 清理目标：删除输出目录
# .PHONY: clean
# clean:
# 	rm -rf target
K=kernel
U=user
A=src/kernal/arch
B=src/kernel/boot
L=src/kernal/lib
LO=src/kernal/lock
SK=src/kernal
# OBJS = \
#   $K/entry.o \
#   $K/start.o \
#   $K/main.o \
#   $K/printf.o\
#   $K/uart.o\
#   $K/spinlock.o\
#   $K/proc.o\

OBJS = \
  $B/entry.o \
  $B/start.o \
  $(SK)/main.o \
  $L/printf.o\
  $L/uart.o\
  $L/cpu.o\
  $(LO)/spinlock.o\

# Try to infer the correct TOOLPREFIX if not set
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-elf-'; \
	elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-elf-'; \
	elif riscv64-none-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-none-elf-'; \
	elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-linux-gnu-'; \
	elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-linux-gnu-'; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

QEMU = qemu-system-riscv64
MIN_QEMU_VERSION = 7.2

CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

CFLAGS = -Wall -Werror -Wno-unknown-attributes -O -fno-omit-frame-pointer -ggdb -gdwarf-2
CFLAGS += -march=rv64gc
CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding
CFLAGS += -fno-common -nostdlib
CFLAGS += -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset
CFLAGS += -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero
CFLAGS += -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc
CFLAGS += -fno-builtin-free
CFLAGS += -fno-builtin-memcpy -Wno-main
CFLAGS += -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf
CFLAGS += -I.
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

LDFLAGS = -z max-page-size=4096

$K/kernel: $(OBJS) $K/kernel.ld
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o $K/kernel $(OBJS) 
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$K/%.o: $K/%.S
	$(CC) -march=rv64gc -g -c -o $@ $<

tags: $(OBJS)
	etags kernel/*.S kernel/*.c

-include kernel/*.d user/*.d

clean: 
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym \
	$K/kernel fs.img \
	mkfs/mkfs .gdbinit \
        $U/usys.S \
	$(UPROGS)

ifndef CPUS
CPUS := 1
endif

QEMUOPTS = -machine virt -bios none -kernel $K/kernel -m 128M -smp $(CPUS) -nographic

qemu:	
	$(QEMU) $(QEMUOPTS)

QEMU_VERSION := $(shell $(QEMU) --version | head -n 1 | sed -E 's/^QEMU emulator version ([0-9]+\.[0-9]+)\..*/\1/')
check-qemu-version:
	@if [ "$(shell echo "$(QEMU_VERSION) >= $(MIN_QEMU_VERSION)" | bc)" -eq 0 ]; then \
		echo "ERROR: Need qemu version >= $(MIN_QEMU_VERSION)"; \
		exit 1; \
	fi