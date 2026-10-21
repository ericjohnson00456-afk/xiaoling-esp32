#include "vad_probability_policy.h"

#include <array>
#include <cassert>

using micro_wake_word::VadQuantizedCutoff;
using micro_wake_word::VadWindowHasSpeech;

int main() {
    constexpr float scale = 1.0f / 256.0f;
    const float cutoff = VadQuantizedCutoff(0.5f, scale, 0);
    assert(cutoff == 127 * scale);
    assert(VadQuantizedCutoff(0.5f, scale, 128) == -scale);
    std::array<float, 5> scores{0, 0, 0, 0, 255 * scale};
    assert(!VadWindowHasSpeech(scores.data(), scores.size(), cutoff));
    scores.fill(127 * scale);
    assert(!VadWindowHasSpeech(scores.data(), scores.size(), cutoff));
    scores[4] = 128 * scale;
    assert(VadWindowHasSpeech(scores.data(), scores.size(), cutoff));
    scores.fill(240 * scale);
    assert(VadWindowHasSpeech(scores.data(), scores.size(), cutoff));
    scores.fill(0);
    assert(!VadWindowHasSpeech(scores.data(), scores.size(), cutoff));
    assert(!VadWindowHasSpeech(nullptr, 0, cutoff));
    // Exhaustively compare a changing five-score window with ESPHome's raw
    // uint8 sum rule, including the exact boundary and zero-padded startup.
    std::array<int, 5> raw{};
    for (int i = 0; i < 4096; ++i) {
        const size_t slot = static_cast<size_t>(i) % raw.size();
        raw[slot] = (i * 73) % 256;
        scores[slot] = raw[slot] * scale;
        int sum = 0;
        for (int p : raw) sum += p;
        assert(VadWindowHasSpeech(scores.data(), scores.size(), cutoff) == (sum > 127 * 5));
    }
}
