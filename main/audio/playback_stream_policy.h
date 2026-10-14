#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

// 首帧保持小读取，持续播放一次多取几个压缩帧；不超过 IO 提供的缓冲容量。
inline size_t StreamingReadBytes(size_t capacity, bool pcm_started) {
    return std::min(capacity, pcm_started ? size_t{1024} : size_t{256});
}

// 取走队尾并不等于扬声器断流。仅在确实等不到后续帧时进入恢复缓冲。
inline bool ShouldRebufferPlayback(uint32_t grace_ms, uint32_t waited_ms,
                                   bool empty, bool same_session, bool finished,
                                   bool output_started) {
    return empty && same_session && !finished && output_started && waited_ms >= grace_ms;
}
