#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Caller provides storage and synchronization. Push is all-or-nothing so an
// overloaded writer can stop at a continuous PCM prefix instead of hiding gaps.
class WakeRecordingBuffer {
public:
    void Reset(int16_t* storage, size_t capacity) {
        storage_ = storage;
        capacity_ = capacity;
        read_ = size_ = 0;
    }

    bool Push(const int16_t* pcm, size_t samples) {
        if (samples > capacity_ - size_) return false;
        if (samples == 0) return true;
        const size_t write = (read_ + size_) % capacity_;
        const size_t first = std::min(samples, capacity_ - write);
        std::memcpy(storage_ + write, pcm, first * sizeof(int16_t));
        std::memcpy(storage_, pcm + first, (samples - first) * sizeof(int16_t));
        size_ += samples;
        return true;
    }

    size_t Pop(int16_t* pcm, size_t capacity) {
        const size_t samples = std::min(size_, capacity);
        if (samples == 0) return 0;
        const size_t first = std::min(samples, capacity_ - read_);
        std::memcpy(pcm, storage_ + read_, first * sizeof(int16_t));
        std::memcpy(pcm + first, storage_, (samples - first) * sizeof(int16_t));
        read_ = (read_ + samples) % capacity_;
        size_ -= samples;
        return samples;
    }

    size_t Size() const { return size_; }

private:
    int16_t* storage_ = nullptr;
    size_t capacity_ = 0;
    size_t read_ = 0;
    size_t size_ = 0;
};
