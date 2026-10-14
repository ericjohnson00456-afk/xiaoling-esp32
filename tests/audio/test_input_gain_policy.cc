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
    std::puts("input gain: S3 reference, dual MIC, unchanged REF and saturation passed");
}
