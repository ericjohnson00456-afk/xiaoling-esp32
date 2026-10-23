#include "audio/input_gain_policy.h"

#include <cassert>
#include <cstdio>

int main() {
    assert(SelectMicrophonePcmGain(true, true) == 2);
    assert(SelectMicrophonePcmGain(true, false) == 5);
    assert(SelectMicrophonePcmGain(false, true) == 5);

    std::vector<int16_t> mic_ref = {1000, 2000, 20000, -3000};
    assert(AmplifyMicrophonePcm(mic_ref, 2, true, 2) == 1);
    assert((mic_ref == std::vector<int16_t>{2000, 2000, 32767, -3000}));

    std::vector<int16_t> dual_mic = {1000, -1000, 8000, -8000};
    assert(AmplifyMicrophonePcm(dual_mic, 2, false, 5) == 2);
    assert((dual_mic == std::vector<int16_t>{5000, -5000, 32767, -32768}));

    std::vector<int16_t> mono = {1500, -1500};
    assert(AmplifyMicrophonePcm(mono, 1, false, 5) == 0);
    assert((mono == std::vector<int16_t>{7500, -7500}));

    // 上行追加 x3：验证正常幅值、静音、正负削波边界及负满刻度，防止溢出翻转。
    std::vector<int16_t> uplink = {
        0, 1, -1, 1000, -1000, 10922, -10922, 10923, -10923, 32767, -32768,
    };
    assert(AmplifyUplinkPcm(uplink) == 4);
    assert((uplink == std::vector<int16_t>{
        0, 3, -3, 3000, -3000, 32766, -32766, 32767, -32768, 32767, -32768,
    }));
    std::vector<int16_t> empty;
    assert(AmplifyUplinkPcm(empty) == 0);
    std::puts("input gain: MIC/REF routing, uplink x3, signed boundaries and saturation passed");
}
