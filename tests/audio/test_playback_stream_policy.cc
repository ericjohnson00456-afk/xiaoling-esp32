#include "audio/playback_stream_policy.h"
#include "audio/audio_output.h"
#include "audio/audio_yield_policy.h"
#include <cassert>
#include <cstdio>

int main() {
    assert(StreamingReadBytes(1024, false, true) == 128);
    assert(StreamingReadBytes(1024, true, true) == 128);
    assert(StreamingReadBytes(1024, true, false) == 1024);
    assert(StreamingReadBytes(128, true, true) == 128);
    assert(StreamingReadBytes(0, false, false) == 0);
    AudioYieldPolicy yield_policy;
    assert(yield_policy.DelayTicks(101, 10) == 0);
    assert(yield_policy.DelayTicks(103, 10) == 0);
    assert(yield_policy.DelayTicks(111, 10) == 1); // 每 20 ms 一帧，错过 phase==0 也会让步。
    assert(yield_policy.DelayTicks(113, 10) == 0);
    assert(yield_policy.DelayTicks(121, 10) == 1);
    assert(yield_policy.DelayTicks(129, 0) == 0);
    assert(kAudioTtsInitialPrebufferBytes < kAudioTtsProducerResumeBytes);
    assert(kAudioTtsRecoveryPrebufferBytes < kAudioTtsProducerResumeBytes);
    assert(kAudioTtsProducerResumeBytes < kAudioTtsMaxBufferedPcmBytes);
    assert(kAudioTtsMaxBufferedPcmBytes == 96 * 1024);
    assert(kAudioTtsInitialPrebufferBytes == 32 * 1024);
    assert(kAudioTtsRecoveryPrebufferBytes == 16 * 1024);
    assert(kAudioTtsPrebufferMaxWaitMs == 1000);
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
