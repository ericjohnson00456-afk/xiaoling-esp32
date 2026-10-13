#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// AGC 输出保护独立于具体 DSP 实现，便于验证静音、增益上限和峰值边界。
namespace SpeechAgcGuard {
inline constexpr double kMaxGain = 3.9810717055349722; // +12 dB
inline constexpr int32_t kPeakLimit = 29204;           // -1 dBFS

inline double Power(const std::vector<int16_t>& pcm) {
    double power = 0.0;
    for (const int16_t value : pcm) power += static_cast<double>(value) * value;
    return power;
}

inline void Apply(std::vector<int16_t>& input,
                  const std::vector<int16_t>& candidate,
                  float previous_mix, float next_mix) {
    if (input.empty() || input.size() != candidate.size()) return;
    previous_mix = std::clamp(previous_mix, 0.0f, 1.0f);
    next_mix = std::clamp(next_mix, 0.0f, 1.0f);
    const double input_power = Power(input);
    const double candidate_power = Power(candidate);
    const double max_power = input_power * kMaxGain * kMaxGain;
    const double candidate_scale = candidate_power > 0.0
        ? std::min(1.0, std::sqrt(max_power / candidate_power)) : 1.0;
    for (size_t i = 0; i < input.size(); ++i) {
        const double mix = previous_mix + (next_mix - previous_mix) *
                           static_cast<double>(i + 1) / input.size();
        const double value = input[i] * (1.0 - mix) +
                             candidate[i] * candidate_scale * mix;
        input[i] = static_cast<int16_t>(std::clamp(
            std::lround(value), -static_cast<long>(kPeakLimit),
            static_cast<long>(kPeakLimit)));
    }
    // 平滑权重随样本改变，最后再次检查整帧能量，确保额外增益不超上限。
    const double output_power = Power(input);
    if (output_power > max_power && output_power > 0.0) {
        const double scale = std::sqrt(max_power / output_power);
        for (auto& sample : input) {
            sample = static_cast<int16_t>(std::lround(sample * scale));
        }
    }
}
}  // namespace SpeechAgcGuard
