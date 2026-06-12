#include "mod.h"

device_t device_table[N_DEVICE];

/* 标准输入设备 */
static uint32 device_stdin_read(uint32 len, uint64 dst, bool is_user_dst)
{
    return cons_read(len, dst, is_user_dst);
}

/* 标准输出设备 */
static uint32 device_stdout_write(uint32 len, uint64 src, bool is_user_src)
{
    return cons_write(len, src, is_user_src);
}

/* 标准错误输出设备 */
static uint32 device_stderr_write(uint32 len, uint64 src, bool is_user_src)
{
    printf("ERROR: ");
    return cons_write(len, src, is_user_src);
}

/* 无限0流 */
static uint32 device_zero_read(uint32 len, uint64 dst, bool is_user_dst)
{
    uint32 write_len = 0, cut_len = 0;

    uint64 src = (uint64)pmem_alloc(true);
    proc_t *p = myproc();

    while (write_len < len)
    {
        cut_len = MIN(len - write_len, PGSIZE);

        if (is_user_dst)
            uvm_copyout(p->pgtbl, dst, src, cut_len);
        else
            memmove((void *)dst, (void *)src, cut_len);

        dst += cut_len;
        write_len += cut_len;
    }

    pmem_free(src, true);

    return write_len;
}

/* 空设备读取 */
static uint32 device_null_read(uint32 len, uint64 dst, bool is_user_dst)
{
    return 0;
}

/* 空设备写入 */
static uint32 device_null_write(uint32 len, uint64 src, bool is_user_src)
{
    return len;
}

/* 彩蛋: 笨蛋GPT */
static uint32 device_gpt0_write(uint32 len, uint64 src, bool is_user_src)
{
    char tmp[STR_MAXLEN + 1];
    proc_t *p = myproc();

    tmp[len] = '\0';

    if (is_user_src)
        uvm_copyin(p->pgtbl, (uint64)tmp, src, len);
    else
        memmove(tmp, (void *)src, len);

    if (strncmp(tmp, "Hello", len) == 0)
    {
        printf("Hi, I am gpt0!\n");
    }
    else if (strncmp(tmp, "Guess who I am", len) == 0)
    {
        printf("Your procid is %d and name is %s.\n", p->pid, p->name);
    }
    else if (strncmp(tmp, "How many free memory left", len) == 0)
    {
        uint32 kernel_free_pages, user_free_pages;
        pmem_stat(&kernel_free_pages, &user_free_pages);
        printf("We have %d free pages in kernel space, %d free pages in user space!\n",
               kernel_free_pages, user_free_pages);
    }
    else if (strncmp(tmp, "Good job", len) == 0)
    {
        printf("Thanks for your kind words!\n");
    }
    else
    {
        printf("Sorry, I can not understand it.\n");
    }

    return len;
}

/* 注册设备 */
static void device_register(uint32 index, char *name,
                            uint32 (*read)(uint32, uint64, bool),
                            uint32 (*write)(uint32, uint64, bool))
{
    memmove(device_table[index].name, name, MAXLEN_FILENAME);
    device_table[index].read = read;
    device_table[index].write = write;
}

/* 初始化device_table */
void device_init()
{
    /* 初始化device_table数组 */
    for (int i = 0; i < N_DEVICE; i++)
    {
        device_table[i].name[0] = 0;
        device_table[i].read = NULL;
        device_table[i].write = NULL;
    }

    /* 注册六种设备 */
    device_register(INODE_MAJOR_STDIN, "stdin", device_stdin_read, NULL);
    device_register(INODE_MAJOR_STDOUT, "stdout", NULL, device_stdout_write);
    device_register(INODE_MAJOR_STDERR, "stderr", NULL, device_stderr_write);
    device_register(INODE_MAJOR_ZERO, "zero", device_zero_read, NULL);
    device_register(INODE_MAJOR_NULL, "null", device_null_read, device_null_write);
    device_register(INODE_MAJOR_GPT0, "gpt0", NULL, device_gpt0_write);

    /* 确保 /dev 目录存在 */
    inode_t *dev_dir = path_to_inode("/dev");
    if (dev_dir == NULL)
    {
        /* 创建 /dev 目录 */
        dev_dir = path_create_inode("/dev", INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
        if (dev_dir == NULL)
            panic("device_init: cannot create /dev");
        /* 保持dev_dir的引用，不要put */
    }

    /* 在 /dev 下创建各个设备文件的inode (dev_dir持有锁和引用) */
    char *dev_names[] = {"stdin", "stdout", "stderr", "zero", "null", "gpt0"};
    uint16 dev_majors[] = {INODE_MAJOR_STDIN, INODE_MAJOR_STDOUT, INODE_MAJOR_STDERR,
                           INODE_MAJOR_ZERO, INODE_MAJOR_NULL, INODE_MAJOR_GPT0};

    inode_lock(dev_dir);

    for (int i = 0; i < 6; i++)
    {
        /* 检查是否已存在 */
        uint32 inum = dentry_search(dev_dir, dev_names[i]);
        if (inum == INVALID_INODE_NUM)
        {
            /* 创建新的设备inode */
            inode_t *ip = inode_create(INODE_TYPE_DIVICE, dev_majors[i], INODE_MINOR_DEFAULT);

            /* 在dev目录中创建目录项 */
            if (dentry_create(dev_dir, ip->inode_num, dev_names[i]) == (uint32)-1)
                panic("device_init: cannot create device dentry");

            inode_put(ip);
        }
    }

    /* 将dev目录inode的变化写回磁盘 */
    inode_rw(dev_dir, true);
    inode_unlock(dev_dir);
    inode_put(dev_dir);
}

/* 检查文件major字段的合法性 */
bool device_open_check(uint16 major, uint32 open_mode)
{
    /* 检查major是否在有效范围 */
    if (major < INODE_MAJOR_STDIN || major > INODE_MAJOR_GPT0)
        return false;

    uint32 index = major;

    /* 检查读权限 */
    if (open_mode & FILE_OPEN_READ)
    {
        if (device_table[index].read == NULL)
            return false;
    }

    /* 检查写权限 */
    if (open_mode & FILE_OPEN_WRITE)
    {
        if (device_table[index].write == NULL)
            return false;
    }

    return true;
}

/* 从设备文件中读取数据 */
uint32 device_read_data(uint16 major, uint32 len, uint64 dst, bool is_user_dst)
{
    if (major >= N_DEVICE || device_table[major].read == NULL)
        return 0;

    return device_table[major].read(len, dst, is_user_dst);
}

/* 向设备文件写入数据 */
uint32 device_write_data(uint16 major, uint32 len, uint64 src, bool is_user_src)
{
    if (major >= N_DEVICE || device_table[major].write == NULL)
        return 0;

    return device_table[major].write(len, src, is_user_src);
}
