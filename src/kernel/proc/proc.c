#include "mod.h"
#include "../../user/initcode.h"

#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t *old, context_t *new);

// in trap/trap_user.c
extern void trap_user_return();

/* ------------本地变量----------- */

// 进程结构体数组 + 第一个用户进程的指针
static proc_t proc_list[N_PROC];
static proc_t *proczero;

// 全局pid + 保护它的锁
static int global_pid;
static spinlock_t pid_lk;

/* 获取一个pid */
static int alloc_pid()
{
	int tmp = 0;
	spinlock_acquire(&pid_lk);
	assert(global_pid > 0, "alloc_pid: overflow");
	tmp = global_pid++;
	spinlock_release(&pid_lk);
	return tmp;
}

/* 释放进程锁 + trap_user_return */
static void proc_return()
{
	proc_t *p = myproc();
	static bool fs_inited = false;

	// 先释放进程锁, 因为fs_init中可能触发proc_sleep
	spinlock_release(&p->lk);

	if (!fs_inited)
	{
		fs_init();
		fs_inited = true;
	}

	trap_user_return();
}

/* 进程模块初始化 */
void proc_init()
{
	// 初始化全局pid锁
	spinlock_init(&pid_lk, "pid_lk");
	global_pid = 1;

	// 初始化进程数组
	for (proc_t *p = proc_list; p < &proc_list[N_PROC]; p++)
	{
		spinlock_init(&p->lk, "proc_lk");
		p->state = UNUSED;
		p->pid = 0;
		p->kstack = KSTACK((int)(p - proc_list));
		p->pgtbl = NULL;
		p->tf = NULL;
		p->parent = NULL;
		p->exit_code = 0;
		p->sleep_space = NULL;
		p->heap_top = 0;
		p->ustack_npage = 0;
		p->mmap = NULL;
		memset(p->name, 0, 16);
		memset(&p->ctx, 0, sizeof(p->ctx));
	}
}

/*
	申请一个UNUSED进程结构体(返回时带锁)
	并执行通用的初始化逻辑
*/
proc_t *proc_alloc()
{
	proc_t *p;

	for (p = proc_list; p < &proc_list[N_PROC]; p++)
	{
		spinlock_acquire(&p->lk);
		if (p->state == UNUSED)
		{
			goto found;
		}
		else
		{
			spinlock_release(&p->lk);
		}
	}
	return NULL;

found:
	// 设置pid
	p->pid = alloc_pid();

	// 分配trapframe页面 (内核内存)
	p->tf = (trapframe_t *)pmem_alloc(true);
	memset(p->tf, 0, PGSIZE);

	// 创建用户页表
	p->pgtbl = proc_pgtbl_init((uint64)p->tf);
	if (p->pgtbl == NULL)
	{
		pmem_free((uint64)p->tf, true);
		p->tf = NULL;
		spinlock_release(&p->lk);
		return NULL;
	}

	// 初始化mmap链表
	p->mmap = NULL;

	// 初始化用户堆和栈
	p->heap_top = 0;
	p->ustack_npage = 0;

	// 初始化进程关系
	p->parent = NULL;
	p->exit_code = 0;
	p->sleep_space = NULL;

	// 设置进程名称
	memset(p->name, 0, 16);

	// 设置上下文: 新进程首次被调度时从proc_return开始执行
	memset(&p->ctx, 0, sizeof(p->ctx));
	p->ctx.ra = (uint64)proc_return;
	p->ctx.sp = p->kstack + PGSIZE;

	// 设置状态为RUNNABLE
	p->state = RUNNABLE;

	return p;
}

/*
	回收一个进程结构体并释放它包含的资源
	tips: 调用者需要持有进程锁
*/
void proc_free(proc_t *p)
{
	// 释放用户页表 (内部会释放trapframe物理页)
	if (p->pgtbl)
	{
		uvm_destroy_pgtbl(p->pgtbl);
		p->pgtbl = NULL;
		// uvm_destroy_pgtbl 已经释放了 TRAPFRAME 对应的物理页 (即 p->tf)
		p->tf = NULL;
	}

	// 释放mmap链表中的所有节点
	mmap_region_t *r = p->mmap;
	while (r != NULL)
	{
		mmap_region_t *next = r->next;
		mmap_region_free(r);
		r = next;
	}
	p->mmap = NULL;

	// 重置字段
	p->pid = 0;
	p->parent = NULL;
	p->exit_code = 0;
	p->sleep_space = NULL;
	p->heap_top = 0;
	p->ustack_npage = 0;
	memset(p->name, 0, 16);
	memset(&p->ctx, 0, sizeof(p->ctx));

	// 最后设置状态为UNUSED
	p->state = UNUSED;
}

// 获得一个初始化过的用户页表
// 完成trapframe和trampoline的映射
// 参考 xv6: kernel/proc.c proc_pagetable()
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
	// 为用户页表分配一个物理页(内核内存)
	pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
	memset(pgtbl, 0, PGSIZE);

	// 映射trampoline页 (仅supervisor访问, 不加PTE_U)
	// 用户<->内核切换的公共代码, 映射到最高虚拟地址
	vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

	// 映射trapframe页 (仅supervisor访问, 不加PTE_U)
	// 用于保存/恢复用户寄存器状态
	vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);

	return pgtbl;
}

/*
	第一个用户态进程的创建
	它的代码和数据位于initcode.h的initcode数组

	第一个进程的用户地址空间布局:
	trapoline   (1 page)
	trapframe   (1 page)
	ustack      (1 page)
	.......
					<--heap_top
	code + data (1 page)
	empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问

	注意: 用用户空间的地址映射需要标记 PTE_U

	参考 xv6: kernel/proc.c userinit() + allocproc()
*/
void proc_make_first()
{
	// 通过proc_alloc申请进程结构体
	proc_t *p = proc_alloc();
	assert(p != NULL, "proc_make_first: proc_alloc failed");
	proczero = p;

	// 分配用户栈 (4页, 用户内存, 位于trapframe下方)
	// 测试用例需要在栈上分配较大的数组
	p->ustack_npage = 4;
	uint64 ustack_va = TRAPFRAME - PGSIZE;       // 栈在trapframe紧下方
	for (int i = 0; i < 4; i++)
	{
		uint64 ustack_pa = (uint64)pmem_alloc(false); // 用户物理内存
		uint64 va = TRAPFRAME - (i + 1) * PGSIZE;
		vm_mappages(p->pgtbl, va, ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
	}

	// 加载initcode到用户空间 USER_BASE处
	uint64 code_pa = (uint64)pmem_alloc(false); // 用户物理内存
	vm_mappages(p->pgtbl, USER_BASE, code_pa, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);
	memmove((void *)code_pa, initcode, initcode_len);
	p->heap_top = USER_BASE + PGSIZE;

	// 设置用户态入口PC = USER_BASE (initcode起始地址)
	p->tf->user_to_kern_epc = USER_BASE;
	// 用户栈初始栈顶
	p->tf->sp = ustack_va + PGSIZE;

	// 设置进程名称
	const char *name = "proczero";
	memmove(p->name, name, 9);

	// 解锁, 让调度器来调度
	spinlock_release(&p->lk);
}

/*
	父进程产生子进程
	UNUSED -> RUNNABLE
*/
int proc_fork()
{
	proc_t *p = myproc();
	proc_t *np;

	// 申请子进程结构体
	np = proc_alloc();
	if (np == NULL)
	{
		return -1;
	}

	// 复制父进程的用户内存
	uvm_copy_pgtbl(p->pgtbl, np->pgtbl, p->heap_top, p->ustack_npage, p->mmap);

	// 复制父进程的堆栈信息
	np->heap_top = p->heap_top;
	np->ustack_npage = p->ustack_npage;

	// 复制父进程的mmap链表
	// (uvm_copy_pgtbl已经复制了物理页, 这里需要复制mmap链表结构)
	mmap_region_t *r = p->mmap;
	mmap_region_t **tail = &np->mmap;
	while (r != NULL)
	{
		mmap_region_t *new_reg = mmap_region_alloc();
		new_reg->begin = r->begin;
		new_reg->npages = r->npages;
		new_reg->next = NULL;
		*tail = new_reg;
		tail = &new_reg->next;
		r = r->next;
	}

	// 复制trapframe
	*(np->tf) = *(p->tf);

	// 设置子进程返回值为0
	np->tf->a0 = 0;

	// 记录父子关系
	np->parent = p;

	// 复制进程名称
	memmove(np->name, p->name, 16);

	// 记录子进程pid
	int pid = np->pid;

	// 解锁子进程
	spinlock_release(&np->lk);

	return pid;
}

/*
	进程主动放弃CPU控制权
	RUNNING->RUNNABLE
*/
void proc_yield()
{
	proc_t *p = myproc();
	spinlock_acquire(&p->lk);
	p->state = RUNNABLE;
	proc_sched();
	spinlock_release(&p->lk);
}

/*
	当父进程退出时, 让它的所有子进程认proczero为父
	因为proczero永不退出, 可以回收子进程的资源
*/
static void proc_reparent(proc_t *parent)
{
	for (proc_t *pp = proc_list; pp < &proc_list[N_PROC]; pp++)
	{
		if (pp->parent == parent)
		{
			// 此处不持有pp->lk, 但因为我们是父进程, 只有我们能修改pp->parent
			spinlock_acquire(&pp->lk);
			pp->parent = proczero;
			spinlock_release(&pp->lk);
		}
	}
}

/*
	唤醒等待呼叫的进程
	由proc_exit调用
	tips: 调用者需要持有parent->lk
*/
static void proc_try_wakeup(proc_t *p)
{
	// 唤醒等待该进程退出的父进程
	// 父进程调用proc_wait时, 会以自己为资源sleep
	// 调用者必须已经持有 parent->lk
	proc_t *parent = p->parent;
	if (parent != NULL)
	{
		if (parent->sleep_space == parent && parent->state == SLEEPING)
		{
			parent->state = RUNNABLE;
			printf("proc %d is wakeup!\n", parent->pid);
		}
	}
}

/*
	进程退出
	RUNNING -> ZOMBIE
*/
void proc_exit(int exit_code)
{
	proc_t *p = myproc();

	if (p == proczero)
		panic("proc_exit: proczero exiting");

	// 将所有子进程过继给proczero
	proc_reparent(p);

	// 获取父进程信息后再获取锁
	spinlock_acquire(&p->lk);
	proc_t *parent = p->parent;
	spinlock_release(&p->lk);

	// 先唤醒父进程 (遵循父->子的锁顺序)
	spinlock_acquire(&parent->lk);

	spinlock_acquire(&p->lk);

	// 设置退出状态和ZOMBIE状态
	p->exit_code = exit_code;
	p->state = ZOMBIE;

	// 唤醒可能正在等待的父进程
	proc_try_wakeup(p);

	// 释放锁
	spinlock_release(&parent->lk);

	// 切换到调度器, 永不返回
	proc_sched();
	panic("proc_exit: zombie exit");
}

/*
	父进程等待一个子进程进入ZOMBIE状态
	1. 如果等到: 释放子进程, 返回子进程的pid, 将子进程的退出状态传出到user_addr
	2. 如果发现没孩子: 返回-1
	3. 如果没等到: 父进程进入睡眠状态
*/
int proc_wait(uint64 user_addr)
{
	proc_t *p = myproc();
	proc_t *np;
	int havekids, pid;

	spinlock_acquire(&p->lk);

	for (;;)
	{
		// 扫描进程数组, 寻找已退出的子进程
		havekids = 0;
		for (np = proc_list; np < &proc_list[N_PROC]; np++)
		{
			if (np->parent == p)
			{
				spinlock_acquire(&np->lk);
				havekids = 1;
				if (np->state == ZOMBIE)
				{
					// 找到ZOMBIE子进程
					pid = np->pid;

					// 将退出状态传出到用户空间
					if (user_addr != 0)
					{
						uvm_copyout(p->pgtbl, user_addr, (uint64)&np->exit_code, sizeof(np->exit_code));
					}

					// 释放子进程资源
					proc_free(np);
					spinlock_release(&np->lk);
					spinlock_release(&p->lk);
					return pid;
				}
				spinlock_release(&np->lk);
			}
		}

		// 没有子进程, 返回-1
		if (!havekids)
		{
			spinlock_release(&p->lk);
			return -1;
		}

		// 没有找到ZOMBIE子进程, 父进程进入睡眠
		proc_sleep(p, &p->lk);
	}
}

/*
	进程等待sleep_space对应的资源, 进入睡眠状态
	RUNNING -> SLEEPING
*/
void proc_sleep(void *sleep_space, spinlock_t *lock)
{
	proc_t *p = myproc();

	// 必须先获取p->lk才能修改p->state
	// 一旦我们持有p->lk, 就不会错过任何wakeup

	if (lock != &p->lk)
	{
		spinlock_acquire(&p->lk);
		spinlock_release(lock);
	}

	// 进入睡眠
	p->sleep_space = sleep_space;
	p->state = SLEEPING;
	printf("proc %d is sleeping!\n", p->pid);

	proc_sched();

	// 被唤醒后清理
	p->sleep_space = NULL;

	// 重新获取原来的锁
	if (lock != &p->lk)
	{
		spinlock_release(&p->lk);
		spinlock_acquire(lock);
	}
}

/*
	唤醒所有等待sleep_space的进程
	SLEEPING -> RUNNABLE
*/
void proc_wakeup(void *sleep_space)
{
	for (proc_t *p = proc_list; p < &proc_list[N_PROC]; p++)
	{
		spinlock_acquire(&p->lk);
		if (p->state == SLEEPING && p->sleep_space == sleep_space)
		{
			p->state = RUNNABLE;
			printf("proc %d is wakeup!\n", p->pid);
		}
		spinlock_release(&p->lk);
	}
}

/*
	用户进程切换到调度器
	tips: 调用者保证持有当前进程的锁
*/
void proc_sched()
{
	proc_t *p = myproc();

	if (!spinlock_holding(&p->lk))
		panic("proc_sched: not holding p->lk");
	if (mycpu()->noff != 1)
		panic("proc_sched: locks");
	if (p->state == RUNNING)
		panic("proc_sched: running");
	if (intr_get())
		panic("proc_sched: interruptible");

	// 切换到调度器上下文
	swtch(&p->ctx, &mycpu()->ctx);
}

/*
	调度器
	RUNNABLE->RUNNING
*/
void proc_scheduler()
{
	cpu_t *c = mycpu();
	c->proc = NULL;

	for (;;)
	{
		// 避免因设备中断导致死锁
		intr_on();

		int found = 0;
		for (proc_t *p = proc_list; p < &proc_list[N_PROC]; p++)
		{
			spinlock_acquire(&p->lk);
			if (p->state == RUNNABLE)
			{
				// 选择该进程运行
				p->state = RUNNING;
				c->proc = p;
				printf("proc %d is running...\n", p->pid);
				swtch(&c->ctx, &p->ctx);

				// 进程已经通过proc_sched切回调度器
				c->proc = NULL;
				found = 1;
			}
			spinlock_release(&p->lk);
		}

		if (found == 0)
		{
			// 没有可运行的进程, 等待中断
			intr_on();
			asm volatile("wfi");
		}
	}
}
