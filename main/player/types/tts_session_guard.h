#pragma once

#include <cstdint>

// 新 URL 已有 pending session 时，它优先于尚未收尾的旧 active session。
inline bool IsCurrentTtsSession(uint32_t session, uint32_t active, uint32_t pending) {
    return session != 0 && session == (pending != 0 ? pending : active);
}
