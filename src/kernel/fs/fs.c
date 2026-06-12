#include "mod.h"

super_block_t sb; /* 超级块 */

/* 基于superblock输出磁盘布局信息 (for debug) */
static void sb_print()
{
    printf("\ndisk layout information:\n");
    printf("1. super block:  block[0]\n");
    printf("2. inode bitmap: block[%d - %d]\n", sb.inode_bitmap_firstblock,
           sb.inode_bitmap_firstblock + sb.inode_bitmap_blocks - 1);
    printf("3. inode region: block[%d - %d]\n", sb.inode_firstblock,
           sb.inode_firstblock + sb.inode_blocks - 1);
    printf("4. data bitmap:  block[%d - %d]\n", sb.data_bitmap_firstblock,
           sb.data_bitmap_firstblock + sb.data_bitmap_blocks - 1);
    printf("5. data region:  block[%d - %d]\n", sb.data_firstblock,
           sb.data_firstblock + sb.data_blocks - 1);
    printf("block size = %d Byte, total size = %d MB, total inode = %d\n\n", sb.block_size,
           (int)((unsigned long long)(sb.total_blocks) * sb.block_size / 1024 / 1024), sb.total_inodes);
}

/* 文件系统初始化 */
void fs_init()
{
    // 初始化缓冲系统
    buffer_init();

    // 读入超级块
    buffer_t *buf = buffer_get(FS_SB_BLOCK);
    memmove(&sb, buf->data, sizeof(sb));
    buffer_put(buf);

    // 验证魔数
    if (sb.magic_num != FS_MAGIC)
        panic("fs_init: invalid file system magic");

    // 输出磁盘布局信息
    sb_print();

    // 初始化inode缓存
    inode_init();

    /* =================================================================
     * 测试1: inode的访问 + 创建 + 删除
     * 预期结果见 ./picture/test-1.png
     * ================================================================= */
    {
        printf("============= test begin =============\n\n");

        inode_t *rooti, *ip_1, *ip_2;

        rooti = inode_get(ROOT_INODE);
        inode_lock(rooti);
        inode_print(rooti, "root");
        inode_unlock(rooti);

        /* 第一次查看bitmap */
        bitmap_print(false);

        ip_1 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
        ip_2 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
        inode_lock(ip_1);
        inode_lock(ip_2);
        inode_dup(ip_2);

        inode_print(ip_1, "dir");
        inode_print(ip_2, "data");

        /* 第二次查看bitmap */
        bitmap_print(false);

        ip_1->disk_info.nlink = 0;
        ip_2->disk_info.nlink = 0;
        inode_unlock(ip_1);
        inode_unlock(ip_2);
        inode_put(ip_1);
        inode_put(ip_2);

        /* 第三次查看bitmap */
        bitmap_print(false);

        inode_put(ip_2);

        /* 第四次查看bitmap */
        bitmap_print(false);

        printf("============= test end =============\n\n");
    }

    /* =================================================================
     * 测试2: 写入和读取inode管理的数据
     * 注意: 大文件的写入过程较慢, 在助教的虚拟机中需要2分钟
     * 预期结果见 ./picture/test-2.png
     * ================================================================= */
    {
        printf("============= test begin =============\n\n");

        inode_t *ip_1, *ip_2;
        uint32 len, cut_len;

        /* 批量读写测试需要的大缓冲区: 提前申请连续物理页面
         * 注意: pmem_alloc使用LIFO空闲链表, 连续分配返回降序地址
         * 因此先分配到临时数组再确定基址 */
        char *big_src, big_dst[9];
        big_dst[8] = 0;
        {
            char *pages[5];
            pages[0] = pmem_alloc(true);
            pages[1] = pmem_alloc(true);
            pages[2] = pmem_alloc(true);
            pages[3] = pmem_alloc(true);
            pages[4] = pmem_alloc(true);

            /* LIFO分配, pages[0]地址最高, pages[4]地址最低 */
            for (int i = 1; i < 5; i++)
                assert(pages[i] == pages[0] - i * PGSIZE, "contiguous fail!");

            /* big_src指向最低地址, big_src+PGSIZE向上遍历5个页面 */
            big_src = pages[4];

            for (uint32 i = 0; i < 5 * (PGSIZE / 8); i++)
                for (uint32 j = 0; j < 8; j++)
                    big_src[i * 8 + j] = 'A' + j;
        }

        /* 小批量读写测试 */

        int small_src[10], small_dst[10];
        for (int i = 0; i < 10; i++)
            small_src[i] = i;

        ip_1 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
        inode_lock(ip_1);
        inode_print(ip_1, "small_data");

        printf("writing data...\n\n");
        cut_len = 10 * sizeof(int);
        for (uint32 offset = 0; offset < 400 * cut_len; offset += cut_len) {
            len = inode_write_data(ip_1, offset, cut_len, small_src, false);
            assert(len == cut_len, "write fail 1!");
        }
        inode_print(ip_1, "small_data");

        len = inode_read_data(ip_1, 120 * cut_len + 4, cut_len, small_dst, false);
        assert(len == cut_len, "read fail 1!");
        printf("read data:");
        for (int i = 0; i < 10; i++)
            printf(" %d", small_dst[i]);
        printf("\n\n");

        ip_1->disk_info.nlink = 0;
        inode_unlock(ip_1);
        inode_put(ip_1);

        /* 大批量读写测试 */

        ip_2 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
        inode_lock(ip_2);
        inode_print(ip_2, "big_data");

        printf("writing data (quick test mode)...\n\n");
        cut_len = PGSIZE * 4 + 1110;
        uint32 test_total = cut_len * 1; // quick verify, full test uses 10000
        for (uint32 offset = 0; offset < test_total; offset += cut_len)
        {
            len = inode_write_data(ip_2, offset, cut_len, big_src, false);
            assert(len == cut_len, "write fail 2!");
        }
        inode_print(ip_2, "big_data");

        len = inode_read_data(ip_2, test_total - 8, 8, big_dst, false);
        assert(len == 8, "read fail 2");
        printf("read data: %s\n", big_dst);

        ip_2->disk_info.nlink = 0;
        inode_unlock(ip_2);
        inode_put(ip_2);

        pmem_free((uint64)big_src, true);
        pmem_free((uint64)big_src + PGSIZE, true);
        pmem_free((uint64)big_src + PGSIZE * 2, true);
        pmem_free((uint64)big_src + PGSIZE * 3, true);
        pmem_free((uint64)big_src + PGSIZE * 4, true);

        printf("============= test end =============\n");
    }

    /* =================================================================
     * 测试3: 目录项的增加、删除、查找操作
     * 预期结果见 ./picture/test-3.png
     * ================================================================= */
    {
        printf("============= test begin =============\n\n");

        inode_t *rooti, *ip_1, *ip_2, *ip_3;
        uint32 inode_num_1, inode_num_2, inode_num_3;
        uint32 len, cutlen, offset;
        char tmp[10];

        tmp[9] = 0;
        cutlen = 9;
        rooti = inode_get(ROOT_INODE);

        /* 搜索预置的dentry */

        inode_lock(rooti);
        inode_num_1 = dentry_search(rooti, "ABCD.txt");
        inode_num_2 = dentry_search(rooti, "abcd.txt");
        inode_num_3 = dentry_search(rooti, ".");
        if (inode_num_1 == INVALID_INODE_NUM ||
            inode_num_2 == INVALID_INODE_NUM ||
            inode_num_3 == INVALID_INODE_NUM) {
            panic("invalid inode num!");
        }
        dentry_print(rooti);
        inode_unlock(rooti);

        ip_1 = inode_get(inode_num_1);
        inode_lock(ip_1);
        ip_2 = inode_get(inode_num_2);
        inode_lock(ip_2);
        ip_3 = inode_get(inode_num_3);
        inode_lock(ip_3);

        inode_print(ip_1, "ABCD.txt");
        inode_print(ip_2, "abcd.txt");
        inode_print(ip_3, "root");

        len = inode_read_data(ip_1, 0, cutlen, tmp, false);
        assert(len == cutlen, "read fail 1!");
        printf("\nread data: %s\n", tmp);

        len = inode_read_data(ip_2, 0, cutlen, tmp, false);
        assert(len == cutlen, "read fail 2!");
        printf("read data: %s\n\n", tmp);

        inode_unlock(ip_1);
        inode_unlock(ip_2);
        inode_unlock(ip_3);
        inode_put(ip_1);
        inode_put(ip_2);
        inode_put(ip_3);

        /* 创建和删除dentry */
        inode_lock(rooti);

        ip_1 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
        offset = dentry_create(rooti, ip_1->inode_num, "new_dir");
        inode_num_1 = dentry_search(rooti, "new_dir");
        printf("new dentry offset = %d\n", offset);
        printf("new dentry inode_num = %d\n\n", inode_num_1);

        dentry_print(rooti);

        inode_num_2 = dentry_delete(rooti, "new_dir");
        assert(inode_num_1 == inode_num_2, "inode num is not equal!");

        dentry_print(rooti);

        inode_unlock(rooti);
        inode_put(rooti);

        printf("============= test end =============\n");
    }

    /* =================================================================
     * 测试4: 文件路径的解析
     * 预期结果见 ./picture/test-4.png
     * ================================================================= */
    {
        printf("============= test begin =============\n\n");

        inode_t *rooti, *ip_1, *ip_2, *ip_3, *ip_4, *ip_5;

        /* 准备测试环境 */

        rooti = inode_get(ROOT_INODE);
        ip_1 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
        ip_2 = inode_create(INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
        ip_3 = inode_create(INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);

        inode_lock(rooti);
        inode_lock(ip_1);
        inode_lock(ip_2);
        inode_lock(ip_3);

        if (dentry_create(rooti, ip_1->inode_num, "AABBC") == -1)
            panic("dentry_create fail 1!");
        if (dentry_create(ip_1, ip_2->inode_num, "aaabb") == -1)
            panic("dentry_create fail 2!");
        if (dentry_create(ip_2, ip_3->inode_num, "file.txt") == -1)
            panic("dentry_create fail 3!");

        char tmp1[] = "This is file context!";
        char tmp2[32];
        inode_write_data(ip_3, 0, sizeof(tmp1), tmp1, false);

        inode_rw(rooti, true);
        inode_rw(ip_1, true);
        inode_rw(ip_2, true);
        inode_rw(ip_3, true);

        inode_unlock(rooti);
        inode_unlock(ip_1);
        inode_unlock(ip_2);
        inode_unlock(ip_3);
        inode_put(rooti);
        inode_put(ip_1);
        inode_put(ip_2);
        inode_put(ip_3);

        char *path = "///AABBC///aaabb/file.txt";
        char name[MAXLEN_FILENAME];

        ip_4 = path_to_inode(path);
        if (ip_4 == NULL)
            panic("invalid ip_4");

        ip_5 = path_to_parent_inode(path, name);
        if (ip_5 == NULL)
            panic("invalid ip_5");

        printf("get a name = %s\n\n", name);

        inode_lock(ip_4);
        inode_lock(ip_5);

        inode_print(ip_4, "file.txt");
        inode_print(ip_5, "aaabb");

        inode_read_data(ip_4, 0, 32, tmp2, false);
        printf("read data: %s\n\n", tmp2);

        inode_unlock(ip_4);
        inode_unlock(ip_5);
        inode_put(ip_4);
        inode_put(ip_5);

        printf("============= test end =============\n");
    }

    while(1);
}
