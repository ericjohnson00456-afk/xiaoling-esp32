#include "player/backend/playback_latency_policy.h"
#include "player/backend/decode_yield_policy.h"
#include "audio/audio_output.h"
#include "audio/playback_session_guard.h"
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
    assert(PlaybackDecodePriority(false, true) == 7);
    assert(PlaybackDecodePriority(true, true) == 8);
    assert(PlaybackDecodePriority(false, true, true) == 8);
    assert(PlaybackDecodePriority(false, false, true) == 6);
    assert(TtsDecodeNeedsBoost(false, 8191));
    assert(!TtsDecodeNeedsBoost(false, 8192));
    assert(TtsDecodeNeedsBoost(true, 8192)); // 低水位之后不会立刻降回去。
    assert(TtsDecodeNeedsBoost(true, 16383));
    assert(!TtsDecodeNeedsBoost(true, 16384));
    assert(PlaybackDecodeYieldIntervalUs(false, true) == 10'000);
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
    assert(kAudioTtsInitialPrebufferBytes == 32 * 1024);
    assert(kAudioTtsRecoveryPrebufferBytes == 16 * 1024);
    assert(kAudioTtsPrebufferMaxWaitMs == 1000);
    assert(OwnsPlaybackSession(42, 42));
    assert(!OwnsPlaybackSession(43, 42)); // 旧 TTS 退出不能清除新 tone。
    assert(!OwnsPlaybackSession(0, 42));  // 已取消流不能再次清理活动状态。
    assert(OwnsPlaybackSession(43, 0));   // 显式全局停止仍可撤销所有输出。
    assert(IsNewerPlaybackSession(42, 0));
    assert(IsNewerPlaybackSession(43, 42));
    assert(!IsNewerPlaybackSession(42, 43)); // 旧 start 不能在新 tone 后抢输出。
    assert(!IsNewerPlaybackSession(43, 43));
    assert(!IsNewerPlaybackSession(0, 43));
    assert(IsNewerPlaybackSession(1, 0xfffffff0u));
    assert(!IsNewerPlaybackSession(0xfffffff0u, 1));
    std::puts("playback latency: prompt first-frame output, priority restoration, bounded yield passed");
}
