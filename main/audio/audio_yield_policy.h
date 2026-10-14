#pragma once
#include <cstdint>

// 各算法线程使用同一个 tick 相位，而不是各自计时后交替 sleep。
// 保持约每 100 ms 一 tick 的原预算，让短暂停顿尽量重合，留给 Idle/媒体/UI。
inline uint32_t AudioYieldDelayTicks(uint32_t now_ticks, uint32_t period_ticks) {
    if (period_ticks == 0) return 0;
    return now_ticks % period_ticks == 0 ? 1 : 0;
}
