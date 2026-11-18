#include "../../main/miniapp/miniapp_store.cc"
#include <cassert>
#include <vector>

namespace {
esp_partition_t partition{0x82000};
std::vector<uint8_t> flash(partition.size, 0xff);
int64_t remaining = -1;
bool present = true;
}

const esp_partition_t *esp_partition_find_first(int, int, const char *) {
    return present ? &partition : nullptr;
}
esp_err_t esp_partition_read(const esp_partition_t *, size_t offset, void *data, size_t size) {
    assert(offset + size <= flash.size());
    memcpy(data, flash.data() + offset, size);
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *, size_t offset, const void *data, size_t size) {
    assert(offset + size <= flash.size());
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        if (remaining == 0) return ESP_FAIL;
        if (remaining > 0) --remaining;
        assert((flash[offset + i] & bytes[i]) == bytes[i]);
        flash[offset + i] &= bytes[i];
    }
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t offset, size_t size) {
    assert(offset % kSector == 0 && size % kSector == 0 && offset + size <= flash.size());
    for (size_t i = 0; i < size; ++i) {
        if (remaining == 0) return ESP_FAIL;
        if (remaining > 0) --remaining;
        flash[offset + i] = 0xff;
    }
    return ESP_OK;
}
void *heap_caps_malloc(size_t size, int) { return malloc(size); }
uint32_t esp_crc32_le(uint32_t crc, const uint8_t *data, uint32_t size) {
    crc = ~crc;
    while (size--) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

static miniapp_package_t Package(const char *source, const char *id) {
    miniapp_package_t package{};
    snprintf(package.id, sizeof(package.id), "%s", id);
    snprintf(package.name, sizeof(package.name), "%s", id);
    strcpy(package.version, "1");
    package.size = strlen(source);
    uint8_t digest[16];
    assert(mbedtls_md5(reinterpret_cast<const uint8_t *>(source), package.size, digest) == 0);
    for (unsigned i = 0; i < sizeof(digest); ++i) snprintf(package.hash + 2 * i, 3, "%02x", digest[i]);
    return package;
}

static void Expect(const char *source, const char *id) {
    miniapp_package_t package{};
    char *loaded = nullptr;
    assert(miniapp_store_has_app());
    assert(!miniapp_store_load(&package, &loaded));
    assert(strcmp(package.id, id) == 0 && strcmp(loaded, source) == 0);
    free(loaded);
}

int main() {
    const char *a = "function on_tick() end -- previous";
    const char *b = "function on_tick() end -- candidate";
    const auto pa = Package(a, "old"), pb = Package(b, "new");
    assert(!miniapp_store_has_app());
    assert(!miniapp_store_save(&pa, a));
    Expect(a, "old");
    const auto baseline = flash;
    const int64_t total = 2 * kSector + pb.size + sizeof(Header);
    // Interrupt every byte of publishing the header, and sample erase/data writes.
    for (int64_t cut = 0; cut < total; cut += cut < 2 * int64_t(kSector) - 128 ? 127 : 1) {
        flash = baseline;
        remaining = cut;
        assert(miniapp_store_save(&pb, b));
        remaining = -1;
        Expect(a, "old");
    }
    flash = baseline;
    assert(!miniapp_store_save(&pb, b));
    Expect(b, "new");
    // A corrupt newest source is not restored or exported; use the intact slot.
    flash[SlotSize(&partition) + kSector] ^= 1;
    Expect(a, "old");
    // A second generation update can replace the invalid slot.
    assert(!miniapp_store_save(&pb, b));
    Expect(b, "new");
    assert(miniapp_store_clear());
    assert(!miniapp_store_has_app());
    assert(miniapp_store_save(&pa, a)); // A racing install cannot undo factory reset.
    sealed = false; // Simulated reboot.
    assert(!miniapp_store_has_app());
    present = false;
    assert(!miniapp_store_supported());
    assert(!miniapp_store_save(&pa, a)); // Keep RAM-only boards usable.
    puts("miniapp source store: interrupted writes, corruption and reset passed");
}
