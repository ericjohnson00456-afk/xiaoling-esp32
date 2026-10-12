#ifndef OPUS_WB_ENCODER_H
#define OPUS_WB_ENCODER_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <opus.h>

// 小聆 lschat 的 Opus-WB 编码器：16 kHz、20 ms、16 kbps CBR、关闭 DTX。
class OpusWbEncoder {
public:
    OpusWbEncoder(int sample_rate, int channels, int duration_ms);
    ~OpusWbEncoder();

    int sample_rate() const { return sample_rate_; }
    int duration_ms() const { return duration_ms_; }

    bool Encode(std::vector<int16_t>&& pcm, std::vector<uint8_t>& opus);
    void Encode(std::vector<int16_t>&& pcm,
                std::function<void(std::vector<uint8_t>&& opus)> handler);
    void ResetState();

private:
    static constexpr int kMaxPacketBytes = 1000;

    std::mutex mutex_;
    OpusEncoder* encoder_ = nullptr;
    int sample_rate_;
    int duration_ms_;
    int frame_size_;
    std::vector<int16_t> input_buffer_;
};

#endif // OPUS_WB_ENCODER_H
