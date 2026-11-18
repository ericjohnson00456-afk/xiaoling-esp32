#pragma once
#include <stddef.h>
#include <stdint.h>

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0xff
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef int esp_err_t;
typedef struct { size_t size; } esp_partition_t;
const esp_partition_t *esp_partition_find_first(int, int, const char *);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
esp_err_t esp_partition_write(const esp_partition_t *, size_t, const void *, size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
uint32_t esp_crc32_le(uint32_t, const uint8_t *, uint32_t);
void *heap_caps_malloc(size_t, int);
