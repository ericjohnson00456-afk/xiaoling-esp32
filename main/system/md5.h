#pragma once

#include <esp_rom_md5.h>

#include <cstddef>
#include <cstdint>
#include <limits>

// Existing cloud checksums and miniapp packages use MD5 on the wire and flash.
// The ROM interface preserves that format across the IDF 6 crypto migration.
class Md5 {
public:
    Md5() { esp_rom_md5_init(&context_); }

    void Update(const void* data, size_t size) {
        auto* bytes = static_cast<const uint8_t*>(data);
        while (size > 0) {
            const auto chunk = static_cast<uint32_t>(
                size > UINT32_MAX ? UINT32_MAX : size);
            esp_rom_md5_update(&context_, bytes, chunk);
            bytes += chunk;
            size -= chunk;
        }
    }

    void Finish(uint8_t digest[16]) { esp_rom_md5_final(digest, &context_); }

    static void Digest(const void* data, size_t size, uint8_t digest[16]) {
        Md5 md5;
        md5.Update(data, size);
        md5.Finish(digest);
    }

private:
    md5_context_t context_{};
};
