#include "miniapp_store.h"

#include <esp_crc.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include "system/md5.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {
constexpr uint32_t kMagic = 0x4d415032;
constexpr size_t kSector = 4096;
constexpr size_t kMaximumSource = 262144;
struct Header {
    uint32_t magic;
    uint32_t format;
    uint32_t sequence;
    miniapp_package_t package;
    uint32_t checksum;
};
static_assert(sizeof(Header) <= kSector);
std::mutex mutex;
bool sealed = false;

const esp_partition_t *Partition() {
    const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                     ESP_PARTITION_SUBTYPE_ANY, "miniapp_src");
    if (!partition || partition->size < 2 * (kSector + kMaximumSource)) return nullptr;
    return partition;
}

size_t SlotSize(const esp_partition_t *partition) {
    return (partition->size / 2) & ~(kSector - 1);
}

bool ValidHeader(const Header &header) {
    const auto &package = header.package;
    return header.magic == kMagic && header.format == 1 &&
           header.checksum == esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&header),
                                           offsetof(Header, checksum)) &&
           package.size > 0 && package.size <= kMaximumSource &&
           package.id[0] && memchr(package.id, 0, sizeof(package.id)) &&
           package.version[0] && memchr(package.version, 0, sizeof(package.version)) &&
           memchr(package.name, 0, sizeof(package.name)) && package.hash[32] == 0 &&
           strspn(package.hash, "0123456789abcdef") == 32;
}

bool VerifyFlash(const esp_partition_t *partition, size_t offset,
                 const miniapp_package_t &package) {
    Md5 md5;
    bool ok = true;
    uint8_t buffer[256], digest[16];
    for (size_t done = 0; ok && done < package.size;) {
        const size_t bytes = std::min(sizeof(buffer), size_t(package.size) - done);
        ok = esp_partition_read(partition, offset + done, buffer, bytes) == ESP_OK;
        if (ok) md5.Update(buffer, bytes);
        done += bytes;
    }
    if (ok) md5.Finish(digest);
    char hash[33];
    if (ok) {
        for (unsigned i = 0; i < sizeof(digest); ++i) snprintf(hash + i * 2, 3, "%02x", digest[i]);
        ok = strcmp(hash, package.hash) == 0;
    }
    return ok;
}

int Latest(const esp_partition_t *partition, Header &latest) {
    int selected = -1;
    for (int slot = 0; slot < 2; ++slot) {
        Header header;
        const size_t offset = SlotSize(partition) * slot;
        if (esp_partition_read(partition, offset, &header, sizeof(header)) != ESP_OK ||
            !ValidHeader(header) || !VerifyFlash(partition, offset + kSector, header.package)) continue;
        if (selected < 0 || int32_t(header.sequence - latest.sequence) > 0) {
            latest = header;
            selected = slot;
        }
    }
    return selected;
}
} // namespace

extern "C" bool miniapp_store_supported(void) {
    return Partition() != nullptr;
}

extern "C" bool miniapp_store_has_app(void) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto *partition = Partition();
    Header header{};
    return !sealed && partition && Latest(partition, header) >= 0 &&
           header.package.size <= MINIAPP_SOURCE_MAX;
}

extern "C" const char *miniapp_store_save(const miniapp_package_t *package, const char *source) {
    std::lock_guard<std::mutex> lock(mutex);
    if (sealed) return "device is resetting";
    const auto *partition = Partition();
    if (!partition) return nullptr;
    if (!package || !source || !package->size || package->size > kMaximumSource)
        return "invalid source package";
    Header previous{};
    const int current = Latest(partition, previous);
    const size_t offset = (current == 0 ? 1 : 0) * SlotSize(partition);
    Header header{};
    header.magic = kMagic;
    header.format = 1;
    header.sequence = current < 0 ? 1 : previous.sequence + 1;
    header.package = *package;
    header.checksum = esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&header),
                                  offsetof(Header, checksum));
    if (!ValidHeader(header)) return "invalid source package";
    const size_t erase_size = kSector + ((package->size + kSector - 1) & ~(kSector - 1));
    if (esp_partition_erase_range(partition, offset, erase_size) != ESP_OK ||
        esp_partition_write(partition, offset + kSector, source, package->size) != ESP_OK ||
        !VerifyFlash(partition, offset + kSector, *package)) return "failed to save Lua source";
    // Publish last. Until the complete checked header exists, the old slot wins.
    if (esp_partition_write(partition, offset, &header, sizeof(header)) != ESP_OK)
        return "failed to commit Lua source";
    Header check;
    if (esp_partition_read(partition, offset, &check, sizeof(check)) != ESP_OK ||
        memcmp(&check, &header, sizeof(header)) != 0) return "failed to verify Lua source header";
    return nullptr;
}

extern "C" const char *miniapp_store_load(miniapp_package_t *package, char **source) {
    if (!package || !source) return "invalid source request";
    *source = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (sealed) return "device is resetting";
    const auto *partition = Partition();
    if (!partition) return "source partition unavailable";
    Header header{};
    const int slot = Latest(partition, header);
    if (slot < 0) return "no saved miniapp";
    if (header.package.size > MINIAPP_SOURCE_MAX) return "saved source exceeds runtime budget";
    auto *data = static_cast<char *>(heap_caps_malloc(header.package.size + 1,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) return "not enough memory for Lua source";
    if (esp_partition_read(partition, slot * SlotSize(partition) + kSector,
                           data, header.package.size) != ESP_OK) {
        free(data);
        return "failed to read Lua source";
    }
    data[header.package.size] = 0;
    *package = header.package;
    *source = data;
    return nullptr;
}

extern "C" bool miniapp_store_clear(void) {
    std::lock_guard<std::mutex> lock(mutex);
    sealed = true;
    const auto *partition = Partition();
    if (!partition) return true;
    return esp_partition_erase_range(partition, 0, kSector) == ESP_OK &&
           esp_partition_erase_range(partition, SlotSize(partition), kSector) == ESP_OK;
}
