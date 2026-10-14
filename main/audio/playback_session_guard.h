#pragma once
#include <cstdint>

// 活动队列可能已经被另一条物理播放流水线接管，旧结束事件不能操作新流。
inline bool OwnsPlaybackSession(uint32_t active_session, uint32_t expected_session) {
    return expected_session == 0 || active_session == expected_session;
}

// ID 来自 AudioPlayer 单调分配；保留最新启动 ID，即使取消后活动 ID 已为 0。
// 暂停音乐的恢复走 ResumePlayback，明确允许旧 ID；迟到的旧 start 不允许。
inline bool IsNewerPlaybackSession(uint32_t candidate, uint32_t latest) {
    const uint32_t distance = candidate - latest;
    return candidate != 0 && (latest == 0 || (distance != 0 && distance < 0x80000000u));
}
