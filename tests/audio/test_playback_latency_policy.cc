#include "player/backend/playback_latency_policy.h"
#include "player/backend/decode_yield_policy.h"
#include "audio/audio_output.h"
#include <cassert>
#include <cstdio>

int main() {
    PlaybackRequest request;
    request.channel = PlaybackChannel::kPrompt;
    request.source_type = PlaybackSourceType::kRomfs;
    assert(IsLowLatencyPrompt(request));
    assert(kAudioPromptInitialPrebufferBytes == 0);
    assert(kAudioPromptRecoveryPrebufferBytes <= 1152);
    assert(PlaybackDecodePriority(true) == 8);
    assert(PlaybackDecodePriority(false) == 6);
    request.source_type = PlaybackSourceType::kUrl;
    assert(!IsLowLatencyPrompt(request));
    request.source_type = PlaybackSourceType::kRomfs;
    request.channel = PlaybackChannel::kTts;
    assert(!IsLowLatencyPrompt(request));
    request.channel = PlaybackChannel::kMusic;
    assert(!IsLowLatencyPrompt(request));
    DecodeYieldPolicy policy;
    policy.Reset(0);
    assert(policy.ShouldYield(10000, 1152, false, PlaybackDecodeYieldIntervalUs(true)));
    assert(!policy.ShouldYield(10000, 1152, false, PlaybackDecodeYieldIntervalUs(false)));
    assert(!policy.ShouldYield(10000, 0, true, PlaybackDecodeYieldIntervalUs(true)));
    assert(kAudioPlayerInitialPrebufferBytes == 16 * 1024);
    assert(kAudioTtsInitialPrebufferBytes == 64 * 1024);
    std::puts("playback latency: prompt first-frame output, priority restoration, bounded yield passed");
}
