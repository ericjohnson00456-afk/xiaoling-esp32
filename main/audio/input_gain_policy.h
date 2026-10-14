#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// 带参考声道的 S3 codec 通常已提供较高模拟增益；其它板型保留原有 x5。
inline constexpr int32_t SelectMicrophonePcmGain(bool esp32s3, bool has_reference) {
    return esp32s3 && has_reference ? 2 : 5;
}

// 只放大麦克风，参考声道不参与；双 MIC 且无参考声道时两路一致处理。
inline size_t AmplifyMicrophonePcm(std::vector<int16_t>& pcm, size_t channels,
                                   bool has_reference, int32_t gain) {
    if (channels == 0 || (has_reference && channels == 1)) return 0;
    const size_t microphones = channels - (has_reference ? 1 : 0);
    size_t clipped = 0;
    for (size_t base = 0; base < pcm.size(); base += channels) {
        for (size_t mic = 0; mic < microphones && base + mic < pcm.size(); ++mic) {
            const int32_t amplified = static_cast<int32_t>(pcm[base + mic]) * gain;
            if (amplified < INT16_MIN || amplified > INT16_MAX) ++clipped;
            pcm[base + mic] = static_cast<int16_t>(std::clamp(amplified,
                static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
        }
    }
    return clipped;
}
