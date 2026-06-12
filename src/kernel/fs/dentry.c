#include "mod.h"

/*
    出于简化目的的假设:
    如果inode_disk.type == INODE_TYPE_DIR
    那么inode_disk.size <= BLOCKSIZE (只有inode_disk.index[0]有效)
    也就是说, 单个目录最多包含BLOCKSIZE / sizeof(dentry)个目录项

    另外, INODE_TYPE_DATA要求数据之间没有空隙
    但是对于INODE_TYPE_DIR来说是无法做到的(目录项的删除很常见)
    因此, ip->size代表block中已经使用的空间大小
*/

/*----------------dentry的查找、增加、删除操作-----------------*/

/*
    在目录ip中查找是否存在名字为name的目录项
    如果找到了返回目录项中存储的inode_num
    如果没找到返回INVALID_INODE_NUM
    注意: 调用者需要持有ip->slk
*/
uint32 dentry_search(inode_t *ip, char *name)
{
    assert(sleeplock_holding(&ip->slk), "dentry_search: slk");
    assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_search: not dir");

    /* 目录还未分配数据块 */
    if (ip->disk_info.index[0] == 0)
        return INVALID_INODE_NUM;

    buffer_t *buf = buffer_get(ip->disk_info.index[0]);
    dentry_t *de = (dentry_t *)buf->data;
    uint32 entry_count = BLOCK_SIZE / sizeof(dentry_t);

    for (uint32 i = 0; i < entry_count; i++)
    {
        /* name[0] != 0 表示该槽位有效 */
        if (de[i].name[0] != 0)
        {
            if (strncmp(de[i].name, name, MAXLEN_FILENAME) == 0)
            {
                uint32 result = de[i].inode_num;
                buffer_put(buf);
                return result;
            }
        }
    }

    buffer_put(buf);
    return INVALID_INODE_NUM;
}

/*
    在目录ip中寻找空闲槽位, 插入新的dentry
    如果成功插入则返回这个目录项的偏移量(还需要更新size)
    如果插入失败(没有空间/发生重名)返回-1
    注意: 调用者需要持有ip->slk
*/
uint32 dentry_create(inode_t *ip, uint32 inode_num, char *name)
{
    assert(sleeplock_holding(&ip->slk), "dentry_create: slk");
    assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_create: not dir");

    /* 检查重名 */
    if (dentry_search(ip, name) != INVALID_INODE_NUM)
        return (uint32)-1;

    /* 确保index[0]存在并清零 (buffer cache可能残留已释放块的旧数据) */
    if (ip->disk_info.index[0] == 0)
    {
        ip->disk_info.index[0] = bitmap_alloc_block();
        buffer_t *clr = buffer_get(ip->disk_info.index[0]);
        memset(clr->data, 0, BLOCK_SIZE);
        buffer_write(clr);
        buffer_put(clr);
    }

    buffer_t *buf = buffer_get(ip->disk_info.index[0]);
    dentry_t *de = (dentry_t *)buf->data;
    uint32 entry_count = BLOCK_SIZE / sizeof(dentry_t);

    /* 寻找空闲槽位 */
    for (uint32 i = 0; i < entry_count; i++)
    {
        if (de[i].name[0] == 0)
        {
            /* 拷贝文件名 */
            int name_len = strlen(name);
            int copy_len = name_len < MAXLEN_FILENAME - 1 ? name_len : MAXLEN_FILENAME - 1;
            memmove(de[i].name, name, copy_len);
            de[i].name[copy_len] = 0;
            de[i].inode_num = inode_num;

            uint32 offset = i * sizeof(dentry_t);
            buffer_write(buf);
            buffer_put(buf);

            /* 更新目录大小 */
            if (offset + sizeof(dentry_t) > ip->disk_info.size)
                ip->disk_info.size = offset + sizeof(dentry_t);

            return offset;
        }
    }

    /* 没有空闲槽位 */
    buffer_put(buf);
    return (uint32)-1;
}

/*
    在目录ip下删除名称为name的dentry, 返回它的inode_num
    如果匹配失败或者遇到非法情况返回INVALID_INODE_NUM
    注意: 调用者需要持有ip->slk
*/
uint32 dentry_delete(inode_t *ip, char *name)
{
    assert(sleeplock_holding(&ip->slk), "dentry_delete: slk");
    assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_delete: not dir");

    /* 目录还未分配数据块 */
    if (ip->disk_info.index[0] == 0)
        return INVALID_INODE_NUM;

    buffer_t *buf = buffer_get(ip->disk_info.index[0]);
    dentry_t *de = (dentry_t *)buf->data;
    uint32 entry_count = BLOCK_SIZE / sizeof(dentry_t);

    for (uint32 i = 0; i < entry_count; i++)
    {
        if (de[i].name[0] != 0)
        {
            if (strncmp(de[i].name, name, MAXLEN_FILENAME) == 0)
            {
                uint32 inum = de[i].inode_num;

                /* 清除槽位 */
                de[i].name[0] = 0;
                de[i].inode_num = INVALID_INODE_NUM;
                buffer_write(buf);
                buffer_put(buf);

                return inum;
            }
        }
    }

    buffer_put(buf);
    return INVALID_INODE_NUM;
}

/* 输出目录中所有有效目录项的信息 (for debug) */
void dentry_print(inode_t *ip)
{
    assert(sleeplock_holding(&ip->slk), "dentry_print: slk!");
    assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_print: not dir!");

    dentry_t *de;
    buffer_t *buf;

    if (ip->disk_info.index[0] == 0)
        panic("dentry_print: invalid index[0]!");

    printf("inode_num = %d, dentries:\n", ip->inode_num);

    buf = buffer_get(ip->disk_info.index[0]);
    for (de = (dentry_t *)(buf->data); de < (dentry_t *)(buf->data + BLOCK_SIZE); de++)
    {
        if (de->name[0] != 0)
        {
            printf("dentry: offset = %d, inode_num = %d, name = %s\n",
                   (uint32)((uint8 *)de - buf->data), de->inode_num, de->name);
        }
    }
    buffer_put(buf);

    printf("\n");
}

/*------------------从文件名到文件路径-----------------*/

/*
    Examples:
    get_element("a/bb/c", name) = "bb/c" + name = "a"
    get_element("///aa//bb", name) = "bb" + name = "aa"
    get_element("aaa", name) = "" + name = "aaa"
    get_element("", name) = NULL + name = ""
    get_element("//", name) = NULL + name = ""
*/
static char *get_element(char *path, char *name)
{
    /* 跳过前置的'/' */
    while (*path == '/')
        path++;

    /* 如果遇到末尾了则返回 */
    if (*path == 0)
    {
        name[0] = 0;
        return NULL;
    }

    /* 记录起点位置 */
    char *start = path;

    /* 推进path直到遇到'/'或者到达末尾 */
    while (*path != '/' && *path != 0)
        path++;

    /* 提取到的name的长度 */
    int len = path - start;
    len = MIN(len, MAXLEN_FILENAME - 1);

    /* 设置name */
    memmove(name, start, len);
    name[len] = 0;

    /* 跳过后置的'/' */
    while (*path == '/')
        path++;

    return path;
}

/*
    根据文件路径(/A/B/C)查找对应inode(inode_B or inode_C)
    如果find_parent_inode == true, 返回父节点inode, name为下一级子节点的名字
    如果find_parent_inode == false, 返回子节点inode, name无意义
    如果失败返回NULL
*/
static inode_t *__path_to_inode(char *path, char *name, bool find_parent_inode)
{
    /* 从根节点开始 */
    inode_t *ip = inode_get(ROOT_INODE);

    char component[MAXLEN_FILENAME];

    /* 逐级解析路径 */
    while ((path = get_element(path, component)) != NULL)
    {
        inode_lock(ip);

        /* 必须是目录类型 */
        if (ip->disk_info.type != INODE_TYPE_DIR)
        {
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }

        /*
            如果查找父节点且当前是最后一级:
            停止在当前这一级, 不进入子节点
        */
        if (find_parent_inode && *path == 0)
        {
            /* 将最后一级的名字复制给调用者 */
            int len = strlen(component);
            len = MIN(len, MAXLEN_FILENAME - 1);
            memmove(name, component, len);
            name[len] = 0;

            inode_unlock(ip);
            return ip;
        }

        /* 在当前目录中查找component */
        uint32 inum = dentry_search(ip, component);
        if (inum == INVALID_INODE_NUM)
        {
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }

        /* 进入下一级 */
        inode_unlock(ip);
        inode_put(ip);

        ip = inode_get(inum);
    }

    /*
        如果到达这里时 find_parent_inode == true,
        说明path没有斜杠(如"file.txt"), 没有父节点
    */
    if (find_parent_inode)
    {
        inode_put(ip);
        return NULL;
    }

    /* 返回目标inode (未上锁) */
    return ip;
}

/*
    基于path寻找inode
    失败返回NULL
*/
inode_t *path_to_inode(char *path)
{
    char name[MAXLEN_FILENAME];
    return __path_to_inode(path, name, false);
}

/*
    基于path寻找inode->parent, 将inode->name放入name
    失败返回NULL, 同时name无效
*/
inode_t *path_to_parent_inode(char *path, char *name)
{
    return __path_to_inode(path, name, true);
}
