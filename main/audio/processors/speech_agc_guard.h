#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// AGC 输出保护独立于具体 DSP 实现，便于验证静音、增益上限和峰值边界。
namespace SpeechAgcGuard {
inline constexpr double kMaxGain = 3.9810717055349722; // +12 dB
inline constexpr int32_t kPeakLimit = 29204;           // -1 dBFS

inline uint64_t IntegerPower(const std::vector<int16_t>& pcm) {
    uint64_t power = 0;
    for (const int32_t value : pcm) power += static_cast<uint32_t>(value * value);
    return power;
}

inline double Power(const std::vector<int16_t>& pcm) {
    return static_cast<double>(IntegerPower(pcm));
}

inline int16_t RoundAndLimit(float value) {
    const int32_t rounded = static_cast<int32_t>(value + (value < 0.0f ? -0.5f : 0.5f));
    return static_cast<int16_t>(std::clamp(rounded, -kPeakLimit, kPeakLimit));
}

inline void Apply(std::vector<int16_t>& input,
                  const std::vector<int16_t>& candidate,
                  float previous_mix, float next_mix) {
    if (input.empty() || input.size() != candidate.size()) return;
    previous_mix = std::clamp(previous_mix, 0.0f, 1.0f);
    next_mix = std::clamp(next_mix, 0.0f, 1.0f);
    // 无增益时是实际旁路：保留峰值保护，不扫描候选能量/执行逐样本软 double。
    if (previous_mix == 0.0f && next_mix == 0.0f) {
        for (auto& value : input) {
            value = static_cast<int16_t>(std::clamp(static_cast<int32_t>(value),
                                                   -kPeakLimit, kPeakLimit));
        }
        return;
    }
    const uint64_t input_power = IntegerPower(input);
    if (input_power == 0) {
        std::fill(input.begin(), input.end(), 0);
        return;
    }
    const uint64_t candidate_power = IntegerPower(candidate);
    const double max_power = input_power * kMaxGain * kMaxGain;
    const float candidate_scale = candidate_power > 0
        ? std::min(1.0f, std::sqrt(static_cast<float>(max_power / candidate_power))) : 1.0f;
    const float mix_step = (next_mix - previous_mix) / static_cast<float>(input.size());
    uint64_t output_power = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        const float mix = i + 1 == input.size() ? next_mix
            : previous_mix + mix_step * static_cast<float>(i + 1);
        const float value = input[i] * (1.0f - mix) + candidate[i] * candidate_scale * mix;
        input[i] = RoundAndLimit(value);
        const int32_t sample = input[i];
        output_power += static_cast<uint32_t>(sample * sample);
    }
    // 平滑权重随样本改变，最后再次检查整帧能量，确保额外增益不超上限。
    if (output_power > max_power) {
        const float scale = std::sqrt(static_cast<float>(max_power / output_power));
        for (auto& sample : input) {
            sample = RoundAndLimit(sample * scale);
        }
    }
}
}  // namespace SpeechAgcGuard
