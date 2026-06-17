# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run Commands

- **Build:** `make build`
- **Build & run in QEMU:** `make run`
- **Debug with GDB:** `make debug` (starts QEMU with GDB stub on dynamic port, generates `.gdbinit`)
- **Clean:** `make clean`

**Toolchain:** RISC-V 64 cross-compiler (`riscv64-linux-gnu-gcc`). The host must have this toolchain installed.

`CPUNUM = 2` in the Makefile controls the SMP core count (change to 1 for single-core).

## Architecture Overview

This is a **RISC-V 64 operating system kernel** (ECNU OS Lab, lab-3: interrupts and exceptions). It runs in QEMU `-machine virt`, starting at `0x80000000` in M-mode, then transitions to S-mode and eventually launches a user-space init process.

### Code organization convention

Each subsystem (arch, lib, mem, trap, proc, lock) follows a three-header pattern:
- `type.h` — type definitions and constants
- `method.h` — function declarations
- `mod.h` — umbrella header (`#include "type.h"` + `#include "method.h"` + related modules)

### Boot sequence

1. **`entry.S`** → QEMU jumps to `_entry` at `0x80000000` (M-mode). Sets up per-CPU stack: `sp = CPU_stack + ((hartid + 1) * 4096)`, then calls `start()`.
2. **`start.c`** → Disables paging, saves hartid to `tp` register, delegates all traps to S-mode via `medeleg`/`mideleg`, initializes the M-mode timer, configures PMP for full physical access, fakes S-mode prior state in `mstatus`, sets `mepc = main`, executes `mret` to enter S-mode at `main()`.
3. **`main.c`** → CPU 0 initializes all subsystems: UART/print, physical memory, kernel page table (SV39), kernel trap handler, then creates the first user process. CPU 1+ spin-waits on `started` then initializes per-hart trap state.

### Trap handling — dual-path design

Traps are split into two paths, sharing a single `scause` dispatch:

**Kernel-mode traps** (`trap.S:kernel_vector` → `trap_kernel_handler`):
- Save all 32 registers on the kernel stack, call `trap_kernel_handler`, restore, `sret`.
- Handles S-mode software interrupts (timer) and S-mode external interrupts (PLIC/UART).

**M-mode timer → S-mode software interrupt cascade** (`trap.S:timer_vector`):
- M-mode timer interrupt fires → `timer_vector` increments MTIMECMP, then sets `sip.SSIP` to trigger an S-mode software interrupt → `kernel_vector` → `timer_interrupt_handler()` clears `sip.SSIP`.
- This M→S forwarding design keeps most interrupt logic in S-mode.

**User-mode traps** (`trampoline.S:user_vector` → `trap_user_handler`):
- The trampoline page is mapped at the same virtual address (`TRAMPOLINE = VA_MAX - PGSIZE`) in both kernel and user page tables, enabling safe privilege transitions.
- `user_vector` saves user registers to the process's `trapframe`, switches to the kernel page table and kernel stack, then calls `trap_user_handler`. Return path is `user_return` → restores user registers, switches to user page table, `sret` back to U-mode.

### Memory management

**Physical memory** (`pmem.c`): Free pages form a singly-linked list (the `next` pointer lives inside the free page itself). Two separate regions: `kern_region` (first `KERN_PAGES` pages from `ALLOC_BEGIN`) and `user_region` (the rest). Each protected by a spinlock.

**Virtual memory** (`kvm.c`): SV39 three-level page table (9 bits per level). `kvm_init()` creates the kernel page table with identity mappings for UART, CLINT, PLIC, kernel code/data, and the allocatable region. `kvm_inithart()` loads the page table into `satp` and flushes the TLB.

Key layout constants (defined in `mem/type.h`):
- `TRAMPOLINE` — top of VA space, shared kernel/user trampoline code
- `TRAPFRAME` — one page below trampoline, per-process trap save area
- `KSTACK(procid)` — per-process kernel stack (guard page + stack page pairs)
- `USER_BASE` — user-space starts at `PGSIZE` (page 0 unmapped as guard)

### Process model

`proc_t` (in `proc/type.h`) holds: pid, user page table, heap top, user stack page count, `trapframe` pointer, kernel stack VA, and a `context_t` for kernel-mode context switching.

`context_t` contains callee-saved registers (ra, sp, s0-s11). `swtch.S:swtch(old, new)` saves current context to `old` and loads from `new` — the building block for scheduler context switches.

### User-space interface

`src/user/` contains the syscall ABI: `syscall_arch.h` (inline asm `ecall` wrappers for 0–6 arguments), `sys.h` (variadic `syscall(...)` macro), `syscall_num.h` (syscall number definitions). `initcode.c` is compiled to a binary blob embedded in the kernel as `initcode.h` — it becomes the first user process.

### Locking

`spinlock.c` implements a spinlock with nested push/pop interrupt disable (`noff` counter). `spinlock_acquire` calls `push_off()` (disable interrupts, track nesting depth) before taking the lock; `spinlock_release` restores the original interrupt state via `pop_off()`. Systems are expected to never hold more than one spinlock at a time.

### Key RISC-V specifics

- `tp` register stores `hartid` (copied from `mhartid` at boot)
- `sscratch` stores the current process's `trapframe` pointer during user execution
- `stvec` points to `kernel_vector` (S-mode handler) or `user_vector` (when user process is running)
- All CSR read/write inlines are in `arch/method.h`
