#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace wake_recording {

constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kHeaderBytes = 44;
// Keep the entire WAV within FAT32's maximum file size, with complete s16 samples.
constexpr uint32_t kMaxDataBytes =
    (std::numeric_limits<uint32_t>::max() - kHeaderBytes) & ~uint32_t{1};

inline std::array<uint8_t, kHeaderBytes> WavHeader(uint32_t data_bytes) {
    std::array<uint8_t, kHeaderBytes> header{};
    const auto put16 = [&header](size_t offset, uint16_t value) {
        header[offset] = value & 0xff;
        header[offset + 1] = (value >> 8) & 0xff;
    };
    const auto put32 = [&header](size_t offset, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) header[offset + i] = (value >> (8 * i)) & 0xff;
    };
    std::memcpy(header.data(), "RIFF", 4);
    put32(4, data_bytes + kHeaderBytes - 8);
    std::memcpy(header.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);  // Linear PCM.
    put16(22, 1);  // Mono.
    put32(24, kSampleRate);
    put32(28, kSampleRate * sizeof(int16_t));
    put16(32, sizeof(int16_t));
    put16(34, 16);
    std::memcpy(header.data() + 36, "data", 4);
    put32(40, data_bytes);
    return header;
}

}  // namespace wake_recording
