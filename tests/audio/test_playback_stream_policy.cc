#include "audio/playback_stream_policy.h"
#include "audio/audio_output.h"
#include "audio/audio_yield_policy.h"
#include <cassert>
#include <cstdio>

int main() {
    assert(StreamingReadBytes(1024, false) == 256);
    assert(StreamingReadBytes(1024, true) == 1024);
    assert(StreamingReadBytes(128, true) == 128);
    assert(StreamingReadBytes(0, false) == 0);
    assert(AudioYieldDelayTicks(100, 10) == 1);
    assert(AudioYieldDelayTicks(101, 10) == 0);
    assert(AudioYieldDelayTicks(109, 10) == 0);
    assert(AudioYieldDelayTicks(110, 10) == 1);
    assert(AudioYieldDelayTicks(110, 0) == 0);
    assert(kAudioTtsInitialPrebufferBytes < kAudioTtsProducerResumeBytes);
    assert(kAudioTtsRecoveryPrebufferBytes < kAudioTtsProducerResumeBytes);
    assert(kAudioTtsProducerResumeBytes < kAudioTtsMaxBufferedPcmBytes);
    assert(kAudioTtsMaxBufferedPcmBytes == 32 * 1024);
    const auto grace = kAudioTtsUnderrunGraceMs;
    // 取走队尾、下一帧在 DMA 仍播放时到达：不许人为添加恢复停顿。
    assert(!ShouldRebufferPlayback(grace, 0, true, true, false, true));
    assert(!ShouldRebufferPlayback(grace, grace - 1, true, true, false, true));
    assert(!ShouldRebufferPlayback(grace, grace, false, true, false, true));
    assert(ShouldRebufferPlayback(grace, grace, true, true, false, true));
    assert(!ShouldRebufferPlayback(grace, grace, true, false, false, true));
    assert(!ShouldRebufferPlayback(grace, grace, true, true, true, true));
    assert(!ShouldRebufferPlayback(grace, grace, true, true, false, false));
    std::puts("stream playback: startup/steady reads, transient empty queue, true underrun, EOF/cancel passed");
}
