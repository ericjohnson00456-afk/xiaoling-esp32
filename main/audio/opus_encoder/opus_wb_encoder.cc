#include "opus_wb_encoder.h"

#include <esp_log.h>

#define TAG "OpusWbEncoder"

OpusWbEncoder::OpusWbEncoder(int sample_rate, int channels, int duration_ms)
    : sample_rate_(sample_rate),
      duration_ms_(duration_ms),
      frame_size_(sample_rate / 1000 * channels * duration_ms) {
    int error = OPUS_OK;
    encoder_ = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &error);
    if (encoder_ == nullptr || error != OPUS_OK) {
        ESP_LOGE(TAG, "创建 Opus-WB 编码器失败: %d", error);
        encoder_ = nullptr;
        return;
    }

    // 16 kbps CBR 在 20 ms 下固定产生 40 字节 payload，符合 lschat opus-wb。
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(16000));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR(0));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR_CONSTRAINT(0));
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(0));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
}

OpusWbEncoder::~OpusWbEncoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ != nullptr) {
        opus_encoder_destroy(encoder_);
    }
}

bool OpusWbEncoder::Encode(std::vector<int16_t>&& pcm, std::vector<uint8_t>& opus) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ == nullptr || pcm.size() != static_cast<size_t>(frame_size_)) {
        ESP_LOGE(TAG, "PCM 帧长度错误: samples=%u, expected=%d",
                 static_cast<unsigned>(pcm.size()), frame_size_);
        return false;
    }

    uint8_t packet[kMaxPacketBytes];
    const int packet_size = opus_encode(encoder_, pcm.data(), frame_size_, packet,
                                        sizeof(packet));
    if (packet_size < 0) {
        ESP_LOGE(TAG, "Opus-WB 编码失败: %d", packet_size);
        return false;
    }
    opus.assign(packet, packet + packet_size);
    return true;
}

void OpusWbEncoder::Encode(
    std::vector<int16_t>&& pcm,
    std::function<void(std::vector<uint8_t>&& opus)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ == nullptr) {
        ESP_LOGE(TAG, "Opus-WB 编码器未初始化");
        return;
    }

    if (input_buffer_.empty()) {
        input_buffer_ = std::move(pcm);
    } else {
        input_buffer_.reserve(input_buffer_.size() + pcm.size());
        input_buffer_.insert(input_buffer_.end(), pcm.begin(), pcm.end());
    }

    while (input_buffer_.size() >= static_cast<size_t>(frame_size_)) {
        uint8_t packet[kMaxPacketBytes];
        const int packet_size = opus_encode(encoder_, input_buffer_.data(), frame_size_,
                                            packet, sizeof(packet));
        if (packet_size < 0) {
            ESP_LOGE(TAG, "Opus-WB 编码失败: %d", packet_size);
            return;
        }
        if (handler != nullptr) {
            handler(std::vector<uint8_t>(packet, packet + packet_size));
        }
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + frame_size_);
    }
}
