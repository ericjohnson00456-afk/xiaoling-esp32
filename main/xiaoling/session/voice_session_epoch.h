#pragma once

#include "event/event_epoch.h"
#include <string_view>

namespace Xiaoling {
// 只在用户主动开启/替换对话时推进；全双工自动续期保留当前回复与字幕。
// 静态生命周期也能隔离销毁旧连接后才出队的消息。
inline AppEvent::Epoch voice_session_epoch;

inline bool AcceptsVoiceReply(bool accepting, uint32_t generation,
                              std::string_view request_id, std::string_view reply_id) {
    return accepting && voice_session_epoch.IsCurrent(generation) &&
           !request_id.empty() && !reply_id.empty() && request_id == reply_id;
}
}  // namespace Xiaoling
