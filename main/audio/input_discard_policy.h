#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// 最低位是丢弃开关，其余位是边界代次。短 tone 整段跨过一次 ADC 读取时
// 即使读取前后开关都已关闭，也必须丢弃这一块，不能只检查当前开关。
inline uint32_t NextInputDiscardState(uint32_t state, bool discard) {
    if (((state & 1u) != 0) == discard) return state;
    return ((state + 2u) & ~1u) | (discard ? 1u : 0u);
}

inline bool DiscardCapturedInput(std::vector<int16_t>& pcm, uint32_t before, uint32_t after) {
    if (before == after && (after & 1u) == 0) return false;
    // 给常驻处理器继续喂静音，避免 AFE 积压旧音频；原始 MIC 不进入后续链路。
    std::fill(pcm.begin(), pcm.end(), 0);
    return true;
}
