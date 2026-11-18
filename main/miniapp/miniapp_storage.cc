#include "miniapp.h"
#include "miniapp_storage.h"
#include "miniapp_platform.h"
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {
struct Record {
    uint32_t magic, size;
    int64_t expires;
    char id[MINIAPP_ID_MAX + 1];
    char data[MINIAPP_STORAGE_MAX_BYTES + 1];
};
constexpr uint32_t kMagic = 0x4d415031;
TickType_t written_at[MINIAPP_STORAGE_MAX_APPS]{};
bool written[MINIAPP_STORAGE_MAX_APPS]{};
struct Store {
    nvs_handle_t handle = 0;
    Store() { nvs_open("miniapp", NVS_READWRITE, &handle); }
    ~Store() {
        if (handle)
            nvs_close(handle);
    }
    bool Erase(const char *key) {
        const auto result = nvs_erase_key(handle, key);
        return (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) &&
               nvs_commit(handle) == ESP_OK;
    }
};
int64_t Now() {
    return miniapp_time_valid() ? time(nullptr) : 0;
}
void Key(int slot, char *key) {
    snprintf(key, 16, "save%d", slot);
}
bool Read(Store &store, int slot, Record &record, bool &exists) {
    char key[16];
    Key(slot, key);
    size_t size = sizeof(record);
    auto result = nvs_get_blob(store.handle, key, &record, &size);
    exists = result != ESP_ERR_NVS_NOT_FOUND;
    if (!exists)
        return true;
    if (result != ESP_OK)
        return false;
    if (size != sizeof(record) || record.magic != kMagic ||
        record.size > MINIAPP_STORAGE_MAX_BYTES || !memchr(record.id, 0, sizeof(record.id)) ||
        record.data[record.size] || record.expires <= 0 || (Now() && record.expires <= Now())) {
        exists = false;
        return store.Erase(key);
    }
    return true;
}
} // namespace
extern "C" int miniapp_storage_load(const char *id, char *data, size_t *size) {
    if (!Now())
        return 0;
    Store store;
    auto record = std::make_unique<Record>();
    if (!store.handle || !record)
        return 0;
    for (int i = 0; i < MINIAPP_STORAGE_MAX_APPS; ++i) {
        bool exists;
        if (!Read(store, i, *record, exists))
            return 0;
        if (exists && !strcmp(record->id, id)) {
            *size = record->size;
            memcpy(data, record->data, *size + 1);
            return 1;
        }
    }
    return 0;
}
extern "C" const char *miniapp_storage_save(const char *id, const char *data, size_t size,
                                            uint32_t ttl) {
    if (!Now())
        return "clock_unavailable";
    if (size > MINIAPP_STORAGE_MAX_BYTES || !ttl || ttl > MINIAPP_STORAGE_MAX_TTL)
        return "invalid_data";
    Store store;
    if (!store.handle)
        return "io_error";
    auto record = std::make_unique<Record>();
    if (!record)
        return "no_memory";
    int target = -1;
    for (int i = 0; i < MINIAPP_STORAGE_MAX_APPS; ++i) {
        bool exists;
        if (!Read(store, i, *record, exists))
            return "io_error";
        if (!exists && target < 0)
            target = i;
        if (exists && !strcmp(record->id, id)) {
            target = i;
            break;
        }
    }
    if (target < 0)
        return "storage_full";
    auto ticks = xTaskGetTickCount();
    if (written[target] &&
        ticks - written_at[target] < pdMS_TO_TICKS(MINIAPP_STORAGE_WRITE_INTERVAL_MS))
        return "rate_limited";
    memset(record.get(), 0, sizeof(*record));
    record->magic = kMagic;
    record->size = size;
    record->expires = Now() + ttl;
    snprintf(record->id, sizeof(record->id), "%s", id);
    memcpy(record->data, data, size);
    char key[16];
    Key(target, key);
    if (nvs_set_blob(store.handle, key, record.get(), sizeof(*record)) != ESP_OK ||
        nvs_commit(store.handle) != ESP_OK)
        return "io_error";
    written[target] = true;
    written_at[target] = ticks;
    return nullptr;
}
extern "C" const char *miniapp_storage_clear(const char *id) {
    Store store;
    if (!store.handle)
        return "io_error";
    auto record = std::make_unique<Record>();
    if (!record)
        return "no_memory";
    for (int i = 0; i < MINIAPP_STORAGE_MAX_APPS; ++i) {
        bool exists;
        if (!Read(store, i, *record, exists))
            return "io_error";
        if (exists && !strcmp(record->id, id)) {
            char key[16];
            Key(i, key);
            return store.Erase(key) ? nullptr : "io_error";
        }
    }
    return nullptr;
}
