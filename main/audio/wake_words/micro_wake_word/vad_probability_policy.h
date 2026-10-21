#pragma once

#include <cstddef>
#include <cstdint>

namespace micro_wake_word {

// ESPHome converts the JSON cutoff to uint8 using cutoff * 255, then compares
// raw score sums. Express the same boundary in the model's dequantized units.
inline float VadQuantizedCutoff(float cutoff, float output_scale, int zero_point) {
    return (static_cast<uint8_t>(cutoff * 255.0f) - zero_point) * output_scale;
}

inline bool VadWindowHasSpeech(const float* probabilities, size_t count, float cutoff) {
    if (count == 0) return false;
    float sum = 0;
    for (size_t i = 0; i < count; ++i) sum += probabilities[i];
    return sum > cutoff * count;  // Strict mean comparison, not a maximum.
}

}  // namespace micro_wake_word
