#include "mod.h"

/*--------------------part-1: 关于内核空间<->用户空间的数据传递--------------------*/

// 用户态地址空间[src, src+len) 拷贝至 内核态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 n, va0, pa0;
    pte_t *pte;

    while (len > 0)
    {
        va0 = ALIGN_DOWN(src, PGSIZE);
        pte = vm_getpte(pgtbl, va0, false);
        if (pte == NULL || (*pte & PTE_V) == 0)
            panic("uvm_copyin: invalid va");
        if ((*pte & PTE_U) == 0)
            panic("uvm_copyin: not a user page");
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (src - va0);
        if (n > len)
            n = len;
        memmove((void *)dst, (void *)(pa0 + (src - va0)), n);

        len -= n;
        dst += n;
        src = va0 + PGSIZE;
    }
}

// 内核态地址空间[src, src+len） 拷贝至 用户态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 n, va0, pa0;
    pte_t *pte;

    while (len > 0)
    {
        va0 = ALIGN_DOWN(dst, PGSIZE);
        pte = vm_getpte(pgtbl, va0, false);
        if (pte == NULL || (*pte & PTE_V) == 0)
            panic("uvm_copyout: invalid va");
        if ((*pte & PTE_U) == 0)
            panic("uvm_copyout: not a user page");
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (dst - va0);
        if (n > len)
            n = len;
        memmove((void *)(pa0 + (dst - va0)), (void *)src, n);

        len -= n;
        src += n;
        dst = va0 + PGSIZE;
    }
}

// 用户态字符串拷贝到内核态
// 最多拷贝maxlen字节, 中途遇到'\0'则终止
// 注意: src dst 不一定是 page-aligned
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    uint64 n, va0, pa0;
    pte_t *pte;
    int got_null = 0;

    while (got_null == 0 && maxlen > 0)
    {
        va0 = ALIGN_DOWN(src, PGSIZE);
        pte = vm_getpte(pgtbl, va0, false);
        if (pte == NULL || (*pte & PTE_V) == 0)
            panic("uvm_copyin_str: invalid va");
        if ((*pte & PTE_U) == 0)
            panic("uvm_copyin_str: not a user page");
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (src - va0);
        if (n > maxlen)
            n = maxlen;

        char *p = (char *)(pa0 + (src - va0));
        while (n > 0)
        {
            if (*p == '\0')
            {
                *(char *)dst = '\0';
                got_null = 1;
                break;
            }
            else
            {
                *(char *)dst = *p;
            }
            --n;
            --maxlen;
            p++;
            dst++;
        }

        src = va0 + PGSIZE;
    }

    if (!got_null)
        panic("uvm_copyin_str: string too long or not null-terminated");
}

/*--------------------part-2: mmap_region相关--------------------*/

// 打印以mmap为首的mmap链
// for debug
void uvm_show_mmaplist(mmap_region_t *mmap)
{
    mmap_region_t *tmp = mmap;
    printf("\nalloced mmap_space:\n");
    if (tmp == NULL)
        printf("empty\n");
    while (tmp != NULL)
    {
        printf("alloced mmap_region: %p ~ %p\n", tmp->begin, tmp->begin + tmp->npages * PGSIZE);
        tmp = tmp->next;
    }
}

// 两个 mmap_region 区域合并
// 注意: 保留一个 释放一个 不操作 next 指针
// 由uvm_mmap调用
static void mmap_merge(mmap_region_t *mmap_1, mmap_region_t *mmap_2, bool keep_mmap_1)
{
    // 确保有效和紧临
    assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL");
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin, "mmap_merge: check fail");

    // merge
    if (keep_mmap_1)
    {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    }
    else
    {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}

// 寻找一块足够大的区域(len), 作为 mmap_region
// 由uvm_mmap调用(处理begin==0的情况)
// 成功返回begin, 失败返回0
static uint64 uvm_mmap_find(mmap_region_t *head_mmap, uint64 len, mmap_region_t **p_last_mmap, mmap_region_t **p_tmp_mmap)
{
    uint64 cur = MMAP_BEGIN;
    mmap_region_t *prev = NULL;
    mmap_region_t *curr = head_mmap;

    while (curr != NULL)
    {
        if (cur + len <= curr->begin)
        {
            // 在 cur 和 curr->begin 之间找到足够大的空隙
            *p_last_mmap = prev;
            *p_tmp_mmap = curr;
            return cur;
        }
        cur = curr->begin + curr->npages * PGSIZE;
        prev = curr;
        curr = curr->next;
    }

    // 检查最后一个区域之后的空间
    if (cur + len <= MMAP_END)
    {
        *p_last_mmap = prev;
        *p_tmp_mmap = NULL;
        return cur;
    }

    return 0; // 没有找到足够大的空间
}

// 在用户页表和进程mmap链里新增mmap区域 [begin, begin + npages * PGSIZE)
// 调用者保证begin是page-aligned的, 页面权限为perm
// 注意: 如果start==0, 意味着需要内核自主找一块足够大的空间
// 失败则panic卡死
uint64 uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    proc_t *p = myproc();
    uint64 end;

    // 处理 begin == 0: 自动寻找空间
    if (begin == 0)
    {
        mmap_region_t *last = NULL, *tmp = NULL;
        begin = uvm_mmap_find(p->mmap, (uint64)npages * PGSIZE, &last, &tmp);
        if (begin == 0)
            panic("uvm_mmap: no free space found");
    }

    end = begin + (uint64)npages * PGSIZE;

    // 边界检查
    if (begin < MMAP_BEGIN || end > MMAP_END)
        panic("uvm_mmap: out of mmap range");
    if (begin % PGSIZE != 0)
        panic("uvm_mmap: begin not page-aligned");

    // 检查是否与已有区域重叠
    mmap_region_t *curr = p->mmap;
    while (curr != NULL)
    {
        uint64 r_begin = curr->begin;
        uint64 r_end = curr->begin + curr->npages * PGSIZE;
        if (!(end <= r_begin || begin >= r_end))
            panic("uvm_mmap: overlap with existing region");
        curr = curr->next;
    }

    // 从仓库申请一个 mmap_region_t
    mmap_region_t *new_reg = mmap_region_alloc();
    new_reg->begin = begin;
    new_reg->npages = npages;
    new_reg->next = NULL;

    // 按 begin 有序插入链表
    mmap_region_t *prev = NULL;
    curr = p->mmap;
    while (curr != NULL && curr->begin < begin)
    {
        prev = curr;
        curr = curr->next;
    }

    if (prev == NULL)
    {
        new_reg->next = p->mmap;
        p->mmap = new_reg;
    }
    else
    {
        new_reg->next = prev->next;
        prev->next = new_reg;
    }

    // 尝试与前面的区域合并
    if (prev != NULL && prev->begin + prev->npages * PGSIZE == new_reg->begin)
    {
        mmap_region_t *saved_next = new_reg->next;
        mmap_merge(prev, new_reg, true);
        prev->next = saved_next;
        new_reg = prev;
    }

    // 尝试与后面的区域合并
    mmap_region_t *next = new_reg->next;
    if (next != NULL && new_reg->begin + new_reg->npages * PGSIZE == next->begin)
    {
        mmap_region_t *saved_next = next->next;
        mmap_merge(new_reg, next, true);
        new_reg->next = saved_next;
    }

    // 分配物理页并建立映射
    for (uint64 va = begin; va < end; va += PGSIZE)
    {
        uint64 pa = (uint64)pmem_alloc(false);
        vm_mappages(p->pgtbl, va, pa, PGSIZE, PTE_U | perm);
    }

    return begin;
}

// 在用户页表和进程mmap链里释放mmap区域 [begin, begin + npages * PGSIZE)
// 失败则panic卡死
void uvm_munmap(uint64 begin, uint32 npages)
{
    proc_t *p = myproc();
    uint64 end = begin + (uint64)npages * PGSIZE;

    // 边界检查
    if (begin < MMAP_BEGIN || end > MMAP_END)
        panic("uvm_munmap: out of mmap range");
    if (begin % PGSIZE != 0)
        panic("uvm_munmap: begin not page-aligned");

    mmap_region_t *prev = NULL;
    mmap_region_t *curr = p->mmap;

    while (curr != NULL)
    {
        uint64 r_begin = curr->begin;
        uint64 r_end = curr->begin + curr->npages * PGSIZE;

        if (r_end <= begin)
        {
            // 当前区域完全在释放范围之前, 跳过
            prev = curr;
            curr = curr->next;
        }
        else if (r_begin >= end)
        {
            // 当前区域完全在释放范围之后, 后面的区域也都是, 结束
            break;
        }
        else if (r_begin >= begin && r_end <= end)
        {
            // 当前区域完全包含在释放范围内, 删除整个区域
            vm_unmappages(p->pgtbl, r_begin, r_end - r_begin, true);
            if (prev)
                prev->next = curr->next;
            else
                p->mmap = curr->next;
            mmap_region_t *to_free = curr;
            curr = curr->next;
            mmap_region_free(to_free);
        }
        else if (r_begin < begin && r_end > end)
        {
            // 释放范围在区域中间, 将区域一分为二
            vm_unmappages(p->pgtbl, begin, end - begin, true);

            // 创建右侧新区域
            mmap_region_t *right = mmap_region_alloc();
            right->begin = end;
            right->npages = (r_end - end) / PGSIZE;
            right->next = curr->next;

            // 左侧保留在原区域
            curr->npages = (begin - r_begin) / PGSIZE;
            curr->next = right;
            break;
        }
        else if (r_begin < begin)
        {
            // 区域向左超出释放范围, 截断右侧部分
            vm_unmappages(p->pgtbl, begin, r_end - begin, true);
            curr->npages = (begin - r_begin) / PGSIZE;
            prev = curr;
            curr = curr->next;
        }
        else
        {
            // 区域向右超出释放范围 (r_begin >= begin && r_end > end)
            // 截断左侧部分
            vm_unmappages(p->pgtbl, r_begin, end - r_begin, true);
            curr->begin = end;
            curr->npages = (r_end - end) / PGSIZE;
            // curr 继续留在在链表中, 但后面区域都在 end 之后, 结束
            break;
        }
    }
}

/*------------------part-3: 用户空间heap和stack管理相关------------------*/

// 用户堆空间增加, 返回新的堆顶地址 (注意栈顶最大值限制)
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len, int flag)
{
    uint64 new_heap_top = cur_heap_top + len;

    // 边界检查: 堆顶不能超过 MMAP_BEGIN
    if (new_heap_top > MMAP_BEGIN)
        return cur_heap_top; // 失败, 返回原堆顶

    // 按页分配和映射
    uint64 va = ALIGN_UP(cur_heap_top, PGSIZE);
    while (va < new_heap_top)
    {
        uint64 pa = (uint64)pmem_alloc(false);
        vm_mappages(pgtbl, va, pa, PGSIZE, PTE_U | flag);
        va += PGSIZE;
    }

    return new_heap_top;
}

// 用户堆空间减少, 返回新的堆顶地址
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len)
{
    uint64 new_heap_top;

    // 防止下溢
    if (len > cur_heap_top)
        new_heap_top = USER_BASE + PGSIZE;
    else
        new_heap_top = cur_heap_top - len;

    // 堆顶最低不能低于初始值
    if (new_heap_top < USER_BASE + PGSIZE)
        new_heap_top = USER_BASE + PGSIZE;

    // 无需缩减
    if (new_heap_top >= cur_heap_top)
        return cur_heap_top;

    // 按页释放
    uint64 va = ALIGN_UP(new_heap_top, PGSIZE);
    while (va < cur_heap_top)
    {
        vm_unmappages(pgtbl, va, PGSIZE, true);
        va += PGSIZE;
    }

    return new_heap_top;
}

// 处理函数栈增长导致的page fault事件
// 成功返回new_ustack_npage，失败返回-1
uint64 uvm_ustack_grow(pgtbl_t pgtbl, uint64 old_ustack_npage, uint64 fault_addr)
{
    // 当前栈区间: [TRAPFRAME - old_ustack_npage * PGSIZE, TRAPFRAME)
    uint64 cur_stack_bottom = TRAPFRAME - old_ustack_npage * PGSIZE;

    // fault_addr 必须在栈延伸方向 (低于当前栈底)
    if (fault_addr >= cur_stack_bottom)
        return -1;

    // 不能越过 mmap 区域边界
    uint64 fault_page = ALIGN_DOWN(fault_addr, PGSIZE);
    if (fault_page < MMAP_END)
        return -1;

    // 计算新的栈页面数量
    uint64 new_ustack_npage = (TRAPFRAME - fault_page) / PGSIZE;
    uint64 new_stack_bottom = TRAPFRAME - new_ustack_npage * PGSIZE;

    // 分配并映射新页面
    for (uint64 va = new_stack_bottom; va < cur_stack_bottom; va += PGSIZE)
    {
        uint64 pa = (uint64)pmem_alloc(false);
        vm_mappages(pgtbl, va, pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    }

    return new_ustack_npage;
}

/*----------------------part-4: 用户页表管理相关----------------------*/

// 递归释放 页表占用的物理页 和 页表管理的物理页
// ps: 顶级页表level = 3
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    for (int i = 0; i < (int)(PGSIZE / sizeof(pte_t)); i++)
    {
        pte_t pte = pgtbl[i];
        if ((pte & PTE_V) == 0)
            continue;

        uint64 child_pa = PTE_TO_PA(pte);

        if (PTE_CHECK(pte) && level > 1)
        {
            // 指向低级页表, 递归释放
            destroy_pgtbl((pgtbl_t)child_pa, level - 1);
        }
        else if (!PTE_CHECK(pte) && level == 1)
        {
            // 叶子页表项, 释放对应的物理页
            pmem_free(child_pa, check_inkernel(child_pa));
        }
    }

    // 释放当前页表页本身
    pmem_free((uint64)pgtbl, check_inkernel((uint64)pgtbl));
}

// 页表销毁
void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, true);   // 可以释放，因为trapframe是每个进程独有的
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false); // 不能释放，因为所有进程共用区域
    destroy_pgtbl(pgtbl, 3);
}

// 连续虚拟空间的复制
// 在uvm_copy_pgtbl中使用
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 va, pa, page;
    int flags;
    pte_t *pte;

    for (va = begin; va < end; va += PGSIZE)
    {
        pte = vm_getpte(old, va, false);
        assert(pte != NULL, "uvm_copy_pgtbl: pte == NULL");
        assert((*pte) & PTE_V, "uvm_copy_pgtbl: pte not valid");

        pa = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        page = (uint64)pmem_alloc(false);
        memmove((char *)page, (const char *)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}

// 拷贝页表 (拷贝并不包括 trapframe 和 trampoline)
// 拷贝的页表管理的物理页是原来页表的复制品
void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint64 ustack_npage, mmap_region_t *mmap)
{
    // 拷贝代码+数据区域: USER_BASE ~ heap_top
    copy_range(old, new, USER_BASE, ALIGN_UP(heap_top, PGSIZE));

    // 拷贝用户栈区域
    uint64 stack_bottom = TRAPFRAME - ustack_npage * PGSIZE;
    copy_range(old, new, stack_bottom, TRAPFRAME);

    // 拷贝 mmap 区域
    mmap_region_t *r = mmap;
    while (r != NULL)
    {
        copy_range(old, new, r->begin, r->begin + r->npages * PGSIZE);
        r = r->next;
    }
}
