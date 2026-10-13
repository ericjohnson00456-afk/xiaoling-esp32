#include "speech_agc_guard.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

int main() {
    // 无人声时旁路，不能因为 AGC 候选输出有较高噪声就抬升静音/底噪。
    std::vector<int16_t> quiet(320, 40);
    const auto quiet_before = quiet;
    SpeechAgcGuard::Apply(quiet, std::vector<int16_t>(320, 3000), 0, 0);
    assert(quiet == quiet_before);
    std::vector<int16_t> silence(320, 0);
    SpeechAgcGuard::Apply(silence, std::vector<int16_t>(320, 10000), 1, 1);
    assert(SpeechAgcGuard::Power(silence) == 0);

    // 弱波形即使 DSP 提议放大十倍，最终额外增益也不能超过约 12 dB。
    std::vector<int16_t> weak(320), candidate(320);
    for (size_t i = 0; i < weak.size(); ++i) {
        weak[i] = static_cast<int16_t>(1000 * std::sin(6.28318530718 * i / 40));
        candidate[i] = weak[i] * 10;
    }
    const double input_power = SpeechAgcGuard::Power(weak);
    SpeechAgcGuard::Apply(weak, candidate, 1, 1);
    const double ratio = std::sqrt(SpeechAgcGuard::Power(weak) / input_power);
    assert(ratio > 3.9 && ratio <= SpeechAgcGuard::kMaxGain + 0.002);

    // 极值及近距离大音量保持峰值余量，负满刻度不能整数溢出。
    std::vector<int16_t> loud = {32767, -32768, 30000, -30000};
    const auto loud_candidate = loud;
    SpeechAgcGuard::Apply(loud, loud_candidate, 1, 1);
    for (int32_t sample : loud) assert(std::abs(sample) <= SpeechAgcGuard::kPeakLimit);

    // 人声门控启用时逐样本过渡，避免一帧开始就突然跳到满增益。
    std::vector<int16_t> transition(320, 1000);
    SpeechAgcGuard::Apply(transition, std::vector<int16_t>(320, 3000), 0, 1);
    assert(transition.front() < 1010 && transition.back() == 3000);
    for (size_t i = 1; i < transition.size(); ++i) {
        assert(transition[i] >= transition[i - 1]);
        assert(transition[i] - transition[i - 1] <= 7);
    }
    std::vector<int16_t> release(320, 1000);
    SpeechAgcGuard::Apply(release, std::vector<int16_t>(320, 3000), 1, 0);
    assert(release.front() > 2990 && release.back() == 1000);
    for (size_t i = 1; i < release.size(); ++i) {
        assert(release[i] <= release[i - 1]);
        assert(release[i - 1] - release[i] <= 7);
    }
    std::puts("speech AGC guard: silence, gain cap, peak limit, smoothing passed");
}
