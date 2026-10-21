#include "wake_speech_gate.h"

#include <cassert>
#include <vector>

using micro_wake_word::WakeSpeechGate;

// Deterministic classifier stub tests timing/lifecycle only, not VAD accuracy.
static bool fail_create = false;
static int live_handles = 0;
static std::vector<int16_t> classified_pcm;

extern "C" vad_handle_t vad_create(vad_mode_t mode) {
    assert(mode == VAD_MODE_0);
    if (fail_create) return nullptr;
    ++live_handles;
    return new vad_handle_with_trigger_t{};
}

extern "C" void vad_destroy(vad_handle_t handle) {
    assert(handle);
    --live_handles;
    delete handle;
}

extern "C" vad_state_t vad_process(vad_handle_t handle, int16_t* pcm, int rate, int ms) {
    assert(handle && rate == 16000 && ms == 10);
    classified_pcm.insert(classified_pcm.end(), pcm, pcm + WakeSpeechGate::kFrameSamples);
    // A marker supplies a scripted classifier result; it is not an energy VAD.
    return pcm[0] == 7 ? VAD_SPEECH : VAD_SILENCE;
}

int main() {
    {
        // Generic sample-clock boundary test, independent of model stride.
        WakeSpeechGate gate(15 * 10 + 30);
        std::vector<int16_t> silence(16000, 0);
        std::vector<int16_t> speech(160, 7);
        assert(!gate.HasRecentSpeech());
        gate.Feed(speech.data(), speech.size());  // Uninitialized cannot pass.
        assert(!gate.HasRecentSpeech() && classified_pcm.empty());
        assert(gate.Reset() && live_handles == 1);
        gate.Feed(silence.data(), silence.size());
        assert(!gate.HasRecentSpeech());
        gate.Feed(speech.data(), speech.size());
        assert(gate.HasRecentSpeech());
        // Expiry uses consumed PCM, regardless of queue delay or CPU speed.
        gate.Feed(silence.data(), 2880);
        assert(gate.HasRecentSpeech());  // Includes the 180ms boundary.
        gate.Feed(silence.data(), 1);
        assert(!gate.HasRecentSpeech());

        // A new session/discontinuity clears speech and a partial VAD frame.
        gate.Feed(speech.data(), speech.size());
        assert(gate.Reset() && live_handles == 1);
        assert(!gate.HasRecentSpeech());
        classified_pcm.clear();
        gate.Feed(silence.data(), 159);
        assert(classified_pcm.empty());
        gate.Feed(speech.data(), speech.size());
        assert(!gate.HasRecentSpeech());  // Future partial speech is not used.
        gate.Feed(speech.data(), 1);
        assert(gate.HasRecentSpeech());

        gate.Stop();
        gate.Stop();
        assert(!gate.HasRecentSpeech() && live_handles == 0);
        fail_create = true;
        assert(!gate.Reset());
        gate.Feed(speech.data(), speech.size());
        assert(!gate.HasRecentSpeech());  // Allocation failure cannot bypass.
        fail_create = false;
        assert(gate.Reset());
        gate.Feed(speech.data(), speech.size());
        assert(gate.HasRecentSpeech());
        fail_create = true;
        assert(!gate.Reset() && !gate.HasRecentSpeech() && live_handles == 0);
        fail_create = false;
    }
    assert(live_handles == 0);

    {
        // Regression: [1,3,40] produces one score per THREE 10ms features.
        constexpr size_t history_ms = WakeSpeechGate::HistoryForScoringWindow(10, 3, 10, 30);
        static_assert(history_ms == 330);
        static_assert(WakeSpeechGate::HistoryForScoringWindow(15, 3, 10, 30) == 480);
        static_assert(WakeSpeechGate::HistoryForScoringWindow(10, 1, 10, 30) == 130);
        WakeSpeechGate gate(130);
        std::vector<int16_t> speech(160, 7), silence(16000, 0);
        assert(gate.Reset());
        gate.Feed(speech.data(), speech.size());
        gate.Feed(silence.data(), 3200);
        assert(!gate.HasRecentSpeech());  // Old 130ms expired too soon.
        assert(gate.Reset(history_ms));
        assert(gate.HistoryMs() == history_ms && gate.SpeechAgeMs() == -1);
        gate.Feed(speech.data(), speech.size());
        assert(gate.SpeechFrames() == 1 && gate.ClassifiedFrames() == 1);
        assert(gate.LastFrameMeanSquare() == 49 && gate.LastFramePeak() == 7);
        gate.Feed(silence.data(), 3200);
        assert(gate.HasRecentSpeech() && gate.SpeechAgeMs() == 200);
        gate.Feed(silence.data(), 2080);
        assert(gate.HasRecentSpeech() && gate.SpeechAgeMs() == 330);
        assert(gate.ConsumedSamples() == 5440);
        assert(gate.ClassifiedFrames() == 34 && gate.SpeechFrames() == 1);
        gate.Feed(silence.data(), 1);
        assert(!gate.HasRecentSpeech());
        gate.Stop();
        assert(gate.SpeechAgeMs() == -1 && gate.ClassifiedFrames() == 0);
        assert(gate.Reset());
        std::vector<int16_t> negative_full_scale(160, -32768);
        gate.Feed(negative_full_scale.data(), negative_full_scale.size());
        assert(gate.LastFramePeak() == 32768);
        assert(gate.LastFrameMeanSquare() == 1073741824U);
    }
    assert(live_handles == 0);

    // Input fragmentation and large blocks preserve exactly the same PCM.
    std::vector<int16_t> input(16000);
    for (size_t i = 0; i < input.size(); ++i) input[i] = int16_t(i % 30000);
    for (size_t chunk : {size_t(1), size_t(159), size_t(160), size_t(320), size_t(16000)}) {
        WakeSpeechGate gate(180);
        assert(gate.Reset());
        classified_pcm.clear();
        for (size_t offset = 0; offset < input.size();) {
            const size_t count = std::min(chunk, input.size() - offset);
            gate.Feed(input.data() + offset, count);
            offset += count;
        }
        assert(classified_pcm == input);
    }
    assert(live_handles == 0);
}
