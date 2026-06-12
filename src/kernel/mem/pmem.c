#include "mod.h"

// 内核空间和用户空间的可分配物理页分开描述
static alloc_region_t kern_region, user_region;

#define ackern (spinlock_acquire(&kern_region.lk))
#define acuser (spinlock_acquire(&user_region.lk))
#define rekern (spinlock_release(&kern_region.lk))
#define reuser (spinlock_release(&user_region.lk))

// 检查地址是否在kernel内
bool check_inkernel(uint64 p)
{
    return p >= kern_region.begin && p < kern_region.end;
}

// 物理内存的初始化
// 本质上就是填写kern_region和user_region, 包括基本数值和空闲链表
void pmem_init(void)
{
    kern_region.begin = (uint64)ALLOC_BEGIN;
    kern_region.end = (uint64)ALLOC_BEGIN + KERN_PAGES * PGSIZE;
    spinlock_init(&kern_region.lk, "kpmem_lk");
    uint64 p = (uint64)ALLOC_BEGIN;
    for (; p + PGSIZE <= kern_region.end; p += PGSIZE)
    {
        pmem_free(p, check_inkernel(p));
    }

    user_region.begin = (uint64)kern_region.end;
    user_region.end = (uint64)ALLOC_END;
    spinlock_init(&user_region.lk, "upmem_lk");
    p = (uint64)user_region.begin;
    for (; p + PGSIZE <= user_region.end; p += PGSIZE)
    {
        pmem_free(p, check_inkernel(p));
    }
    return;
}

// 尝试返回一个可分配的清零后的物理页
// 失败则panic锁死
void *pmem_alloc(bool in_kernel)
{
    page_node_t *page;
    if (in_kernel)
    {
        if (!kern_region.allocable)
        {
            buffer_invalidate(N_BUFFER);
            if (!kern_region.allocable)
                panic("alloc kernelmem error!");
        }
        ackern;
        page = kern_region.list_head.next;
        kern_region.list_head.next = kern_region.list_head.next->next;
        kern_region.allocable--;
        memset(page, (uint8)0, PGSIZE);
        // printf("%d\n", kern_region.allocable);
        rekern;
    }
    else
    {
        if (!user_region.allocable)
        {
            panic("alloc usermem error!");
        }
        acuser;
        page = user_region.list_head.next;
        user_region.list_head.next = user_region.list_head.next->next;
        user_region.allocable--;
        memset(page, (uint8)0, PGSIZE);
        reuser;
    }
    return page;
}

// 释放一个物理页
// 失败则panic锁死
void pmem_free(uint64 page, bool in_kernel)
{
    if ((page % PGSIZE) != 0 || page > (uint64)ALLOC_END || page < (uint64)ALLOC_BEGIN)
    {
        panic("freepmem error!");
    }
    memset((void *)page, 0, PGSIZE);
    page_node_t *freepage = (page_node_t *)page;
    if (in_kernel)
    {
        ackern;
        freepage->next = kern_region.list_head.next;
        kern_region.list_head.next = freepage;
        kern_region.allocable++;
        rekern;
    }
    else
    {
        acuser;
        freepage->next = user_region.list_head.next;
        user_region.list_head.next = freepage;
        user_region.allocable++;
        reuser;
    }
}

#define TEST_CNT 10

// 测试目标: 常规申请和释放操作
void test_case_2()
{
    alloc_region_t *user_ar = &user_region;
    uint64 user_pages[TEST_CNT];

    for (int i = 0; i < TEST_CNT; i++)
        user_pages[i] = 0;

    printf("=== test_case_2: Phase 1 - Allocate User Pages ===\n");
    for (int i = 0; i < TEST_CNT; i++)
    {
        user_pages[i] = (uint64)pmem_alloc(false);

        printf("Allocated user page[%d] @ %p\n", i, (void *)user_pages[i]);

        if (!(user_pages[i] >= user_ar->begin && user_pages[i] < user_ar->end))
        {
            printf("Assertion failed: Page address out of bounds! Page: %p, Region: [%p, %p)\n",
                   (void *)user_pages[i], (void *)user_ar->begin, (void *)user_ar->end);
            panic("Page address out of user region bounds");
        }

        memset((void *)user_pages[i], 0xAA, PGSIZE);
    }

    printf("=== test_case_2: Phase 2 - Pre-free Check ===\n");
    spinlock_acquire(&user_ar->lk);
    int expected_before = (user_ar->end - user_ar->begin) / PGSIZE - TEST_CNT;
    int actual = user_ar->allocable;
    printf("Expected allocable: %d, Actual: %d\n", expected_before, actual);
    assert(user_ar->allocable == expected_before, "Allocable count incorrect before free");
    spinlock_release(&user_ar->lk);

    printf("=== test_case_2: Phase 3 - Free Pages ===\n");
    for (int i = 0; i < TEST_CNT; i++)
    {
        pmem_free(user_pages[i], false);
        printf("Free user page[%d] @ %p\n", i, (void *)user_pages[i]);
    }

    printf("=== test_case_2: Phase 4 - Post-free Check ===\n");
    spinlock_acquire(&user_ar->lk);
    int expected_after = (user_ar->end - user_ar->begin) / PGSIZE;
    actual = user_ar->allocable;
    printf("Expected allocable: %d, Actual: %d\n", expected_after, actual);
    assert(user_ar->allocable == expected_after, "Allocable count not restored after free");
    if (user_ar->list_head.next != NULL)
        printf("Free list head @ %p\n", user_ar->list_head.next);
    else
        panic("Free list is empty after freeing pages");
    spinlock_release(&user_ar->lk);

    printf("=== test_case_2: Phase 5 - Reallocate & Verify Zero ===\n");
    for (int i = 0; i < TEST_CNT; i++)
    {
        void *page = pmem_alloc(false);
        printf("Reallocated page[%d] @ %p\n", i, page);

        bool non_zero = false;
        for (int j = 0; j < PGSIZE / sizeof(int); j++)
        {
            if (((int *)page)[j] != 0)
            {
                non_zero = true;
                printf("Non-zero value detected at offset %d: 0x%x value:%d\n", j, ((int *)page)[j], ((int *)page)[j]);
                break;
            }
        }
        assert(!non_zero, "Memory not zeroed after free");
        printf("Zero verification passed\n");
    }

    printf("test_case_2 passed!\n");
}