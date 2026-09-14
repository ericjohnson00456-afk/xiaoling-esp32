#ifndef __ROMFS_H__
#define __ROMFS_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct romfs;

#define ROMFS_PATH_MAX 512

struct romfs_dir_iter {
    struct romfs *fs;
    const void *dir;
    const void *next;
    char base[ROMFS_PATH_MAX];
    uint32_t base_len;
};

int romfs_init(struct romfs **p_fs, const void *base_addr, uint32_t size);
int romfs_deinit(struct romfs **p_fs);

int romfs_info_get(struct romfs *fs, const char *path, uint8_t **data, uint32_t *size);
int romfs_dir_iter_start(struct romfs *fs, const char *dir_path, struct romfs_dir_iter *iter);
int romfs_dir_iter_next(struct romfs_dir_iter *iter, char *path, uint32_t path_size,
                        uint8_t **data, uint32_t *size, bool *is_dir);
bool romfs_exists(struct romfs *fs, const char *path);
bool romfs_isdir(struct romfs *fs, const char *path);
bool romfs_isfile(struct romfs *fs, const char *path);

#ifdef __cplusplus
}
#endif

#endif
