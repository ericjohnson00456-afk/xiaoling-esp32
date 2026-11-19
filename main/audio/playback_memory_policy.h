#pragma once

#include <cstddef>

// Shared-bus playback needs a longer read-ahead window; independent storage
// keeps a smaller working set. Neither profile changes streaming TTS buffering.
struct PlaybackMemoryPolicy {
    size_t file_read_bytes;
    size_t file_buffer_bytes;
    size_t local_prebuffer_bytes;
    size_t local_resume_bytes;
    size_t local_max_bytes;

    static constexpr size_t kInternalPcmLimit = 16 * 1024;
    static constexpr size_t kInternalPrebuffer = 4 * 1024;

    static constexpr PlaybackMemoryPolicy For(size_t psram_bytes, bool shared_spi) {
        if (psram_bytes == 0) {
            return {1024, 8 * 1024, kInternalPrebuffer,
                    8 * 1024, kInternalPcmLimit};
        }
        if (shared_spi && psram_bytes >= 4 * 1024 * 1024) {
            return {16 * 1024, 256 * 1024, 192 * 1024, 384 * 1024, 512 * 1024};
        }
        return {4 * 1024, 32 * 1024, 32 * 1024, 48 * 1024, 64 * 1024};
    }
};
