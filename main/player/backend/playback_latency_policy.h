#pragma once

#include "player/types/playback_request.h"
#include <cstdint>

// 本地提示音必须及时出声；网络 TTS/音乐仍使用后台解码预算。
inline bool IsLowLatencyPrompt(const PlaybackRequest& request) {
    return request.channel == PlaybackChannel::kPrompt &&
           request.source_type == PlaybackSourceType::kRomfs;
}

inline unsigned PlaybackDecodePriority(bool prompt, bool tts = false) {
    return prompt ? 8 : (tts ? 7 : 6);
}
inline int64_t PlaybackDecodeYieldIntervalUs(bool prompt) {
    return prompt ? 10 * 1000 : 50 * 1000;
}
