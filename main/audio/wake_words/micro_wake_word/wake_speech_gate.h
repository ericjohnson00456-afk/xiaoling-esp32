#pragma once

#include <esp_vad.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace micro_wake_word {

// Runs on the same mono 16 kHz samples consumed by the wake frontend, never on
// a newer AFE state. This is an event gate; it must not suppress model input.
class WakeSpeechGate {
public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kFrameMs = 10;  // esp_vad.h supports 10/20/30 ms.
    static constexpr size_t kFrameSamples = kSampleRate * kFrameMs / 1000;

    explicit WakeSpeechGate(size_t history_ms)
        : history_samples_(history_ms * (kSampleRate / 1000)) {}
    ~WakeSpeechGate() { Stop(); }
    WakeSpeechGate(const WakeSpeechGate&) = delete;
    WakeSpeechGate& operator=(const WakeSpeechGate&) = delete;

    bool Reset() {
        Stop();
        // Mode 0 is the least restrictive documented mode: favor quiet speech.
        vad_ = vad_create(VAD_MODE_0);
        return vad_ != nullptr;
    }

    void Stop() {
        if (vad_) vad_destroy(vad_);
        vad_ = nullptr;
        frame_size_ = 0;
        consumed_samples_ = 0;
        last_speech_sample_ = 0;
        has_speech_ = false;
    }

    void Feed(const int16_t* pcm, size_t samples) {
        if (!vad_) return;
        while (samples > 0) {
            const size_t count = std::min(samples, kFrameSamples - frame_size_);
            std::copy_n(pcm, count, frame_.data() + frame_size_);
            frame_size_ += count;
            consumed_samples_ += count;
            pcm += count;
            samples -= count;
            if (frame_size_ == kFrameSamples) {
                if (vad_process(vad_, frame_.data(), kSampleRate, kFrameMs) == VAD_SPEECH) {
                    has_speech_ = true;
                    last_speech_sample_ = consumed_samples_;
                }
                frame_size_ = 0;
            }
        }
    }

    bool HasRecentSpeech() const {
        return vad_ && has_speech_ &&
               consumed_samples_ - last_speech_sample_ <= history_samples_;
    }

private:
    vad_handle_t vad_ = nullptr;
    const size_t history_samples_;
    std::array<int16_t, kFrameSamples> frame_{};
    size_t frame_size_ = 0;
    uint64_t consumed_samples_ = 0;
    uint64_t last_speech_sample_ = 0;
    bool has_speech_ = false;
};

}  // namespace micro_wake_word
