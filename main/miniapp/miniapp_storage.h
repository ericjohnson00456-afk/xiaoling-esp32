#ifdef __cplusplus
extern "C" {
#endif
#ifndef MINIAPP_STORAGE_H
#define MINIAPP_STORAGE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define MINIAPP_STORAGE_MAX_BYTES 1024
#define MINIAPP_STORAGE_MAX_APPS 4
#define MINIAPP_STORAGE_DEFAULT_TTL 604800
#define MINIAPP_STORAGE_MAX_TTL 2592000
#define MINIAPP_STORAGE_WRITE_INTERVAL_MS 10000
/* Called only by the miniapp task. Data is an opaque, bounded UTF-8 JSON object. */
int miniapp_storage_load(const char *id, char *data, size_t *size);
const char *miniapp_storage_save(const char *id, const char *data, size_t size, uint32_t ttl);
const char *miniapp_storage_clear(const char *id);
#endif

#ifdef __cplusplus
}
#endif
