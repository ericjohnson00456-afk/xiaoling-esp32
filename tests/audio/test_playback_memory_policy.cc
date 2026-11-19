#include "audio/playback_memory_policy.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
    for (size_t memory : {0u, 2u * 1024 * 1024, 8u * 1024 * 1024}) {
        for (bool shared : {false, true}) {
            const auto policy = PlaybackMemoryPolicy::For(memory, shared);
            assert(policy.file_read_bytes <= policy.file_buffer_bytes);
            assert(policy.local_prebuffer_bytes <= policy.local_resume_bytes);
            assert(policy.local_resume_bytes < policy.local_max_bytes);
            if (memory == 0) {
                assert(policy.local_max_bytes <= PlaybackMemoryPolicy::kInternalPcmLimit);
                assert(policy.file_buffer_bytes + policy.local_max_bytes <= 24 * 1024);
            } else {
                assert(policy.file_buffer_bytes + policy.local_max_bytes <= memory / 4);
            }
        }
    }
    // Preserve the validated shared-SPI read-ahead and PCM hysteresis.
    const auto shared = PlaybackMemoryPolicy::For(8 * 1024 * 1024, true);
    assert(shared.file_read_bytes == 16 * 1024);
    assert(shared.file_buffer_bytes == 256 * 1024);
    assert(shared.local_prebuffer_bytes == 192 * 1024);
    assert(shared.local_resume_bytes == 384 * 1024);
    assert(shared.local_max_bytes == 512 * 1024);
    const auto independent = PlaybackMemoryPolicy::For(8 * 1024 * 1024, false);
    assert(independent.local_max_bytes < shared.local_max_bytes);
    std::puts("playback memory: bounded internal queues and shared-SPI headroom passed");
}
