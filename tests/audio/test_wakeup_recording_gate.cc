#include "audio/wakeup_recording_gate.h"
#include "audio/input_discard_policy.h"

#include <cassert>
#include <cstdio>
#include <utility>

int main() {
    WakeupRecordingGate gate;
    gate.Begin(1, true);
    assert(!gate.Blocked()); // AEC 保持 tone 和录音并行。
    assert(!gate.Complete(1));
    gate.Begin(2, false);
    assert(gate.Blocked());
    for (int seconds = 0; seconds < 3600; ++seconds) {
        assert(gate.Blocked()); // 无录音/tone 固定超时，任意长音频仍等播放终态。
    }
    assert(!gate.Complete(1)); // 旧 tone 的完成不能开放新录音。
    assert(gate.Blocked());
    assert(gate.Complete(2));
    assert(!gate.Blocked());
    assert(!gate.Complete(2)); // 重复终态不能重新开启录音。
    gate.Begin(3, false);
    gate.Begin(4, false);
    assert(!gate.Complete(3) && gate.Blocked());
    gate.Cancel();
    assert(!gate.Blocked() && !gate.Complete(4));
    gate.Begin(5, false);
    assert(gate.Complete(5)); // 缺失资源/失败时同一完成入口解锁，不永久卡住。

    const auto blocked = NextInputDiscardState(0, true);
    const auto released = NextInputDiscardState(blocked, false);
    assert(blocked & 1u);
    assert((released & 1u) == 0 && released != 0);
    assert(NextInputDiscardState(blocked, true) == blocked);
    assert(NextInputDiscardState(released, false) == released);
    for (const auto size : {320, 640}) { // mono 和 MIC/REF 都不能残留 tone。
        for (const auto& states : {std::pair{0u, blocked}, std::pair{blocked, blocked},
                                  std::pair{blocked, released}, std::pair{0u, released}}) {
            std::vector<int16_t> frame(size, 12345);
            assert(DiscardCapturedInput(frame, states.first, states.second));
            assert(std::all_of(frame.begin(), frame.end(), [](auto sample) { return sample == 0; }));
        }
        std::vector<int16_t> clean(size, 2345);
        assert(!DiscardCapturedInput(clean, released, released));
        assert(clean.front() == 2345 && clean.back() == 2345);
    }
    assert((NextInputDiscardState(0xffffffffu, false) & 1u) == 0);
    std::puts("wakeup gate: AEC parallel, arbitrary tone duration, epoch/cancel, ADC boundary discard passed");
}
