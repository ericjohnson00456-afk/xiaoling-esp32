#pragma once

#include <cstdint>

// Application 主任务使用；释放条件是本轮 tone 的播放终态，不依赖音频长度。
class WakeupRecordingGate {
public:
    void Begin(uint32_t epoch, bool aec_enabled) {
        epoch_ = epoch;
        blocked_ = !aec_enabled;
    }
    bool Blocked() const { return blocked_; }
    bool Complete(uint32_t epoch) {
        if (epoch != epoch_) return false;
        const bool blocked = blocked_;
        Cancel();
        return blocked;
    }
    void Cancel() { epoch_ = 0; blocked_ = false; }

private:
    uint32_t epoch_ = 0;
    bool blocked_ = false;
};
