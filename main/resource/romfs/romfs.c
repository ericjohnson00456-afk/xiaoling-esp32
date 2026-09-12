#define TAG "romfs"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>

#include "romfs.h"

#define ROMBSIZE    (1 << 10)
#define ROMBSBITS   10
#define ROMBMASK    (ROMBSIZE - 1)
#define ROMFS_MAGIC 0x7275

#define ROMFS_MAXFN 128

#ifndef cpu_to_be32
#define cpu_to_be32(x)                                                                                                 \
    (((uint32_t)((x) & 0x000000FF) << 24) | ((uint32_t)((x) & 0x0000FF00) << 8) |                                      \
     ((uint32_t)((x) & 0x00FF0000) >> 8) | ((uint32_t)((x) & 0xFF000000) >> 24))
#endif

#ifndef be32_to_cpu
#define be32_to_cpu(x) cpu_to_be32(x)
#endif

#define ROMFS_ALIGN(x) (((x) + ROMFH_PAD) & ROMFH_MASK)

#ifndef __MIN__
#define __MIN__(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef __MAX__
#define __MAX__(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define __mkw(h, l)       (((h) & 0x00ff) << 8 | ((l) & 0x00ff))
#define __mkl(h, l)       (((h) & 0xffff) << 16 | ((l) & 0xffff))
#define __mk4(a, b, c, d) cpu_to_be32(__mkl(__mkw(a, b), __mkw(c, d)))
#define ROMSB_WORD0       __mk4('-', 'r', 'o', 'm')
#define ROMSB_WORD1       __mk4('1', 'f', 's', '-')

struct romfs_super_block {
    uint32_t word0;
    uint32_t word1;
    uint32_t size;
    uint32_t checksum;
    char name[];
};

struct romfs_inode {
    uint32_t next;
    uint32_t spec;
    uint32_t size;
    uint32_t checksum;
    char name[];
};

struct romfs {
    const uint8_t *base_addr;
    uint32_t size;
    struct romfs_inode *root;
};

#define ROMFH_TYPE 7
#define ROMFH_HRD  0
#define ROMFH_DIR  1
#define ROMFH_REG  2
#define ROMFH_SYM  3
#define ROMFH_BLK  4
#define ROMFH_CHR  5
#define ROMFH_SCK  6
#define ROMFH_FIF  7
#define ROMFH_EXEC 8

#define ROMFH_SIZE 16
#define ROMFH_PAD  (ROMFH_SIZE - 1)
#define ROMFH_MASK (~ROMFH_PAD)

#define PATH_SEPARATOR          '/'

static inline uint32_t romfs_node_type(const struct romfs_inode *node)
{
    if (node == NULL) {
        return 0;
    }
    return (be32_to_cpu(node->next) & ROMFH_TYPE);
}

static uint32_t romfs_checksum(void *data, uint32_t size);

static bool romfs_inode_is_valid(struct romfs *fs, const struct romfs_inode *node)
{
    const uint8_t *base;
    const uint8_t *node_ptr;
    uint32_t name_len;
    uint32_t meta_size;
    uint32_t checksum;
    uint32_t data_size;

    if (fs == NULL || node == NULL || fs->base_addr == NULL || fs->size < ROMFH_SIZE) {
        return false;
    }

    base = fs->base_addr;
    node_ptr = (const uint8_t *)node;
    if (node_ptr < base || (uint32_t)(node_ptr - base) + ROMFH_SIZE > fs->size) {
        return false;
    }

    name_len = strnlen((char *)node->name, ROMFS_MAXFN);
    if (name_len == ROMFS_MAXFN) {
        return false;
    }

    meta_size = ROMFH_SIZE + ROMFS_ALIGN(name_len + 1);
    if ((uint32_t)(node_ptr - base) + meta_size > fs->size) {
        return false;
    }

    checksum = romfs_checksum((void *)node, meta_size);
    if (checksum != 0) {
        return false;
    }

    data_size = be32_to_cpu(node->size);
    if (romfs_node_type(node) == ROMFH_REG || romfs_node_type(node) == ROMFH_SYM) {
        if ((uint32_t)(node_ptr - base) + meta_size + data_size > fs->size) {
            return false;
        }
    } else {
        if (data_size != 0) {
            return false;
        }
    }

    return true;
}

static const struct romfs_inode *romfs_node_resolve(struct romfs *fs, const struct romfs_inode *node)
{
    const struct romfs_inode *current = node;
    int depth = 0;

    if (!romfs_inode_is_valid(fs, current)) {
        return NULL;
    }

    while (current != NULL && romfs_node_type(current) == ROMFH_HRD) {
        uint32_t target_addr = be32_to_cpu(current->spec);
        if (target_addr == 0x00 || (target_addr + sizeof(struct romfs_inode)) > fs->size) {
            return NULL;
        }
        current = (const struct romfs_inode *)(fs->base_addr + target_addr);
        if (!romfs_inode_is_valid(fs, current)) {
            return NULL;
        }
        if (++depth > 8) {
            return NULL;
        }
    }

    return current;
}

static uint32_t romfs_checksum(void *data, uint32_t size)
{
    uint8_t *p = (uint8_t *)data;
    uint32_t sum = 0;
    uint32_t i = 0;

    while (i + 4 <= size) {
        uint32_t word = ((uint32_t)p[i] << 24) | ((uint32_t)p[i + 1] << 16) | ((uint32_t)p[i + 2] << 8) |
                        (uint32_t)p[i + 3];
        sum += word;
        i += 4;
    }

    if (i < size) {
        uint32_t word = 0;
        uint32_t shift = 24;
        while (i < size) {
            word |= ((uint32_t)p[i] << shift);
            shift -= 8;
            i++;
        }
        sum += word;
    }

    return sum;
}

static inline const struct romfs_inode *brother_next(struct romfs *fs, const struct romfs_inode *node)
{
    uint32_t next_addr = be32_to_cpu(node->next) & 0xFFFFFFF0;
    if (next_addr == 0x00 || (next_addr + sizeof(struct romfs_inode)) > fs->size) {
        return NULL;
    }

    const struct romfs_inode *next = (const struct romfs_inode *)(fs->base_addr + next_addr);
    if (!romfs_inode_is_valid(fs, next)) {
        return NULL;
    }

    return next;
}

static inline const struct romfs_inode *child_first(struct romfs *fs, const struct romfs_inode *parent)
{
    if (parent == NULL || fs == NULL) {
        return NULL;
    }

    if (romfs_node_type(parent) != ROMFH_DIR) {
        return NULL;
    }

    uint32_t child_addr = be32_to_cpu(parent->spec);
    if (child_addr == 0x00 || (child_addr + sizeof(struct romfs_inode)) > fs->size) {
        return NULL;
    }

    const struct romfs_inode *child = (const struct romfs_inode *)(fs->base_addr + child_addr);
    if (!romfs_inode_is_valid(fs, child)) {
        return NULL;
    }

    return child;
}

static const struct romfs_inode *romfs_node_find(struct romfs *fs, const struct romfs_inode *dir, const char *name)
{
    if (dir == NULL) {
        return NULL;
    }

    if (romfs_node_type(dir) != ROMFH_DIR) {
        return NULL;
    }

    const struct romfs_inode *child = child_first(fs, dir);
    while (child != NULL) {
        if (strcmp((char *)child->name, name) == 0) {
            return child;
        }

        child = brother_next(fs, child);
    }

    return NULL;
}

static bool romfs_path_init_base(const char *dir_path, char *base, size_t base_size, uint32_t *base_len)
{
    size_t len;
    size_t start = 0;

    if (dir_path == NULL || base == NULL || base_len == NULL) {
        return false;
    }

    len = strlen(dir_path);
    if (len == 0) {
        return false;
    }

    if (dir_path[0] != PATH_SEPARATOR) {
        if (base_size < 2) {
            return false;
        }
        base[0] = PATH_SEPARATOR;
        start = 1;
    }

    if (start + len + 1 > base_size) {
        return false;
    }

    memcpy(base + start, dir_path, len);
    base[start + len] = '\0';

    if (start == 1) {
        len += 1;
    }

    while (len > 1 && base[len - 1] == PATH_SEPARATOR) {
        base[len - 1] = '\0';
        len--;
    }

    *base_len = (uint32_t)len;
    return true;
}

static bool romfs_path_join(const char *base, uint32_t base_len, const char *name, char *out, uint32_t out_size)
{
    size_t name_len;
    size_t total_len;

    if (base == NULL || name == NULL || out == NULL || out_size == 0) {
        return false;
    }

    name_len = strnlen(name, ROMFS_MAXFN);
    if (base_len == 1) {
        total_len = (size_t)base_len + name_len;
        if (total_len + 1 > out_size) {
            return false;
        }
        out[0] = PATH_SEPARATOR;
        memcpy(out + 1, name, name_len);
    } else {
        total_len = (size_t)base_len + 1 + name_len;
        if (total_len + 1 > out_size) {
            return false;
        }
        memcpy(out, base, base_len);
        out[base_len] = PATH_SEPARATOR;
        memcpy(out + base_len + 1, name, name_len);
    }

    out[total_len] = '\0';
    return true;
}

static const struct romfs_inode *romfs_lookup(struct romfs *fs, const struct romfs_inode *root, const char *path)
{
    const struct romfs_inode *current = root;
    char *token;
    char *save_ptr;
    char *path_copy = NULL;

    if (fs == NULL || root == NULL || path == NULL) {
        return NULL;
    }

    if (strlen(path) == 0 || strcmp(path, "/") == 0) {
        return root;
    }

    path_copy = malloc(strlen(path) + 1);
    if (path_copy == NULL) {
        return NULL;
    }

    strncpy(path_copy, path, strlen(path));
    path_copy[strlen(path)] = '\0';

    if (path_copy[0] == PATH_SEPARATOR) {
        token = strtok_r(path_copy + 1, "/", &save_ptr);
    } else {
        token = strtok_r(path_copy, "/", &save_ptr);
    }

    while (token != NULL && current != NULL) {
        if (romfs_node_type(current) != ROMFH_DIR) {
            current = NULL;
            break;
        }

        current = romfs_node_find(fs, current, token);
        if (current == NULL) {
            break;
        }

        token = strtok_r(NULL, "/", &save_ptr);
    }

    free(path_copy);

    return romfs_node_resolve(fs, current);
}

int romfs_init(struct romfs **p_fs, const void *base_addr, uint32_t size)
{
    uint32_t img_size;
    uint32_t img_checksum;
    uint32_t pos;
    uint32_t len;

    if (p_fs == NULL || base_addr == NULL || size == 0) {
        return -1;
    }
    *p_fs = NULL;

    struct romfs *fs = malloc(sizeof(struct romfs));
    if (fs == NULL) {
        ESP_LOGE(TAG, "ROMFS 内存分配失败");
        return -1;
    }

    const struct romfs_super_block *sb = (const struct romfs_super_block *)base_addr;

    if (sb->word0 != ROMSB_WORD0 || sb->word1 != ROMSB_WORD1) {
        ESP_LOGE(TAG, "ROMFS 超级块无效");
        goto fail;
    }

    img_size = be32_to_cpu(sb->size);
    if (img_size < ROMFH_SIZE || img_size > size) {
        ESP_LOGE(TAG, "ROMFS 镜像大小无效");
        goto fail;
    }

    fs->base_addr = (const uint8_t *)base_addr;
    fs->size = img_size;

    img_checksum = romfs_checksum((uint8_t *)sb, __MIN__(512, img_size));
    if (img_checksum != 0) {
        ESP_LOGE(TAG, "ROMFS 校验失败，期望=%08x，实际=%08x",
                 (unsigned int)0, (unsigned int)img_checksum);
        goto fail;
    }

    pos = ROMFH_SIZE;

    len = strnlen((char *)sb->name, ROMFS_MAXFN);
    pos = (ROMFH_SIZE + len + 1 + ROMFH_PAD) & ROMFH_MASK;
    fs->root = (struct romfs_inode *)(base_addr + pos);
    if (!romfs_inode_is_valid(fs, fs->root) || romfs_node_type(fs->root) != ROMFH_DIR) {
        ESP_LOGE(TAG, "ROMFS 根节点无效");
        goto fail;
    }

    *p_fs = fs;
    ESP_LOGI(TAG, "ROMFS 初始化成功，卷=%s，大小=%lu，地址=%p",
             sb->name, (unsigned long)img_size, sb);

    return 0;

fail:
    free(fs);
    return -1;
}

int romfs_deinit(struct romfs **p_fs)
{
    if (p_fs == NULL || *p_fs == NULL) {
        return -1;
    }

    free(*p_fs);
    *p_fs = NULL;

    return 0;
}

int romfs_info_get(struct romfs *fs, const char *path, uint8_t **data, uint32_t *size)
{
    if (data == NULL || size == NULL) {
        return -1;
    }

    const struct romfs_inode *node = romfs_lookup(fs, fs->root, path);
    if (node == NULL) {
        return -1;
    }

    uint32_t offset = sizeof(*node) + ROMFS_ALIGN(strnlen((char *)node->name, ROMFS_MAXFN) + 1);
    *size = be32_to_cpu(node->size);
    *data = ((uint8_t *)node + offset);

    return 0;
}

int romfs_dir_iter_start(struct romfs *fs, const char *dir_path, struct romfs_dir_iter *iter)
{
    const struct romfs_inode *dir;

    if (fs == NULL || dir_path == NULL || iter == NULL) {
        return -1;
    }

    dir = romfs_lookup(fs, fs->root, dir_path);
    if (dir == NULL || romfs_node_type(dir) != ROMFH_DIR) {
        return -1;
    }

    memset(iter, 0, sizeof(struct romfs_dir_iter));

    if (!romfs_path_init_base(dir_path, iter->base, sizeof(iter->base), &iter->base_len)) {
        return -1;
    }

    iter->fs = fs;
    iter->dir = dir;
    iter->next = child_first(fs, dir);

    return 0;
}

int romfs_dir_iter_next(struct romfs_dir_iter *iter, char *path, uint32_t path_size,
                        uint8_t **data, uint32_t *size, bool *is_dir)
{
    const struct romfs_inode *child;

    if (iter == NULL || iter->fs == NULL || iter->dir == NULL || path == NULL || path_size == 0 ||
        data == NULL || size == NULL) {
        return -1;
    }

    *data = NULL;
    *size = 0;

    child = (const struct romfs_inode *)iter->next;
    while (child != NULL) {
        const struct romfs_inode *resolved;
        const char *name = (char *)child->name;
        uint32_t node_type;
        uint32_t offset;

        iter->next = brother_next(iter->fs, child);

        if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            child = (const struct romfs_inode *)iter->next;
            continue;
        }

        resolved = romfs_node_resolve(iter->fs, child);
        if (resolved == NULL) {
            child = (const struct romfs_inode *)iter->next;
            continue;
        }

        if (!romfs_path_join(iter->base, iter->base_len, name, path, path_size)) {
            return -1;
        }

        node_type = romfs_node_type(resolved);
        if (is_dir != NULL) {
            *is_dir = (node_type == ROMFH_DIR);
        }

        if (node_type == ROMFH_REG) {
            offset = sizeof(*resolved) + ROMFS_ALIGN(strnlen((char *)resolved->name, ROMFS_MAXFN) + 1);
            *size = be32_to_cpu(resolved->size);
            *data = ((uint8_t *)resolved + offset);
        }

        return 0;
    }

    return 1;
}

bool romfs_exists(struct romfs *fs, const char *path)
{
    const struct romfs_inode *node = romfs_lookup(fs, fs->root, path);
    return node != NULL;
}

bool romfs_isdir(struct romfs *fs, const char *path)
{
    const struct romfs_inode *node = romfs_lookup(fs, fs->root, path);
    return node != NULL && romfs_node_type(node) == ROMFH_DIR;
}

bool romfs_isfile(struct romfs *fs, const char *path)
{
    const struct romfs_inode *node = romfs_lookup(fs, fs->root, path);
    return node != NULL && romfs_node_type(node) == ROMFH_REG;
}
