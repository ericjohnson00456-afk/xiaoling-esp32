#include "xiaoling/session/voice_session_epoch.h"
#include "event/app_event.h"
#include "player/types/tts_session_guard.h"

#include <cassert>
#include <cstdio>

int main() {
    auto& epoch = Xiaoling::voice_session_epoch;
    for (const auto state : {"recording", "tts_pending", "tts_playing"}) {
        (void)state;
        const auto old_generation = epoch.Current();
        assert(Xiaoling::AcceptsVoiceReply(true, old_generation, "old", "old"));
        assert(!Xiaoling::AcceptsVoiceReply(true, old_generation, "old", ""));
        assert(!Xiaoling::AcceptsVoiceReply(true, old_generation, "old", "other"));
        const AppEvent::EpochSnapshot delayed{&epoch, old_generation};
        epoch.Advance(); // 唤醒替换，旧 TTS/IAT/finish 立即失效。
        assert(!delayed.IsCurrent());
        assert(!Xiaoling::AcceptsVoiceReply(true, old_generation, "old", "old"));
        const auto fresh = epoch.Current();
        assert(!Xiaoling::AcceptsVoiceReply(false, fresh, "old", "old"));
        assert(Xiaoling::AcceptsVoiceReply(true, fresh, "new", "new"));
        assert(!Xiaoling::AcceptsVoiceReply(true, fresh, "new", "old"));
        // 自动全双工续期不推进对话代次；旧 HTTP TTS 可继续，新 WS rid 必须隔离。
        assert((AppEvent::EpochSnapshot{&epoch, fresh}.IsCurrent()));
        assert(!Xiaoling::AcceptsVoiceReply(true, fresh, "renewed", "new"));
        assert(Xiaoling::AcceptsVoiceReply(true, fresh, "renewed", "renewed"));
    }
    // 旧 active 的结束/首帧不能清除新 pending 的等待标志。
    assert(!IsCurrentTtsSession(41, 41, 42));
    assert(IsCurrentTtsSession(42, 41, 42));
    assert(IsCurrentTtsSession(42, 42, 0));
    assert(!IsCurrentTtsSession(41, 42, 0));
    assert(!IsCurrentTtsSession(0, 0, 0));
    std::puts("voice session epoch: recording/TTS restart, stale rid, deferred callbacks, renewal passed");
}
