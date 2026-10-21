#include "audio/wake_recording_buffer.h"
#include "audio/wake_recording_wav.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace {
uint32_t Read32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}
}

int main() {
    // Consumer/writer chunk sizes differ; repeated wraparound must preserve
    // every sample, including silence and the full signed PCM range.
    int16_t storage[997];
    WakeRecordingBuffer buffer;
    buffer.Reset(storage, 997);
    std::vector<int16_t> expected, actual;
    for (int frame = 0; frame < 1000; ++frame) {
        int16_t input[160];
        for (int sample = 0; sample < 160; ++sample) {
            input[sample] = static_cast<int16_t>((frame * 160 + sample) % 65536 - 32768);
        }
        assert(buffer.Push(input, 160));
        expected.insert(expected.end(), input, input + 160);
        if (frame % 3 == 2) {
            int16_t output[512];
            const size_t samples = buffer.Pop(output, 512);
            actual.insert(actual.end(), output, output + samples);
        }
    }
    int16_t tail[997];
    const size_t remaining = buffer.Pop(tail, 997);
    actual.insert(actual.end(), tail, tail + remaining);
    assert(actual == expected);
    assert(buffer.Size() == 0);

    // On backpressure the entire incoming frame is rejected: previously
    // accepted samples must remain intact and in order for finalization.
    int16_t small_storage[4];
    const int16_t first[] = {0, -32768, 32767};
    const int16_t overflow[] = {10, 20};
    buffer.Reset(small_storage, 4);
    assert(buffer.Push(first, 3));
    assert(!buffer.Push(overflow, 2));
    assert(buffer.Size() == 3);
    assert(buffer.Pop(tail, 2) == 2 && tail[0] == 0 && tail[1] == -32768);
    assert(buffer.Push(overflow, 2));
    assert(buffer.Pop(tail, 4) == 3);
    assert(tail[0] == 32767 && tail[1] == 10 && tail[2] == 20);
    buffer.Reset(nullptr, 0);
    assert(!buffer.Push(first, 1));
    assert(buffer.Pop(tail, 4) == 0);

    // Check a real finalized file layout, including header rewrite, duration,
    // format, data offset and sample byte order using an independent reader.
    FILE* file = nullptr;
#ifdef _WIN32
    assert(tmpfile_s(&file) == 0);
#else
    file = std::tmpfile();
#endif
    assert(file != nullptr);
    auto header = wake_recording::WavHeader(0);
    assert(std::fwrite(header.data(), 1, header.size(), file) == 44);
    const uint8_t pcm[] = {0, 0, 0, 128, 255, 127};
    assert(std::fwrite(pcm, 1, sizeof(pcm), file) == sizeof(pcm));
    header = wake_recording::WavHeader(sizeof(pcm));
    assert(std::fseek(file, 0, SEEK_SET) == 0);
    assert(std::fwrite(header.data(), 1, header.size(), file) == 44);
    assert(std::fflush(file) == 0);
    std::rewind(file);
    uint8_t contents[50];
    assert(std::fread(contents, 1, sizeof(contents), file) == sizeof(contents));
    assert(std::memcmp(contents, "RIFF", 4) == 0 && Read32(contents + 4) == 42);
    assert(std::memcmp(contents + 8, "WAVEfmt ", 8) == 0);
    assert(Read32(contents + 16) == 16 && contents[20] == 1 && contents[22] == 1);
    assert(Read32(contents + 24) == 16000 && Read32(contents + 28) == 32000);
    assert(contents[32] == 2 && contents[34] == 16);
    assert(std::memcmp(contents + 36, "data", 4) == 0 && Read32(contents + 40) == 6);
    assert(std::memcmp(contents + 44, pcm, sizeof(pcm)) == 0);
    std::fclose(file);

    header = wake_recording::WavHeader(wake_recording::kMaxDataBytes);
    assert(uint64_t(Read32(header.data() + 4)) + 8 ==
           uint64_t(wake_recording::kMaxDataBytes) + 44);
    assert(Read32(header.data() + 40) % 2 == 0);
    std::puts("wake recording: lossless ring wrap, overflow prefix, WAV format/finalization and size limit passed");
}
