#pragma once
#include <cstdint>

// 各算法线程沿用同一全局 tick 周期。原先只检查 phase==0，20 ms 一帧
// 的任务可能永远踩不到它，持续负载时会饿死 CPU1 Idle。
class AudioYieldPolicy {
public:
    uint32_t DelayTicks(uint32_t now_ticks, uint32_t period_ticks) {
        if (period_ticks == 0) return 0;
        const uint32_t slot = now_ticks / period_ticks;
        if (!initialized_) {
            initialized_ = true;
            last_slot_ = slot;
            return 0;
        }
        if (slot == last_slot_) return 0;
        last_slot_ = slot;
        return 1;
    }

private:
    bool initialized_ = false;
    uint32_t last_slot_ = 0;
};
