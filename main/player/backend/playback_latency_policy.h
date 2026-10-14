#pragma once

#include "player/types/playback_request.h"
#include <cstdint>
#include <cstddef>

// 本地提示音必须及时出声；网络 TTS/音乐仍使用后台解码预算。
inline bool IsLowLatencyPrompt(const PlaybackRequest& request) {
    return request.channel == PlaybackChannel::kPrompt &&
           request.source_type == PlaybackSourceType::kRomfs;
}

inline bool TtsDecodeNeedsBoost(bool boosted, size_t buffered_pcm_bytes) {
    // 低于约 256 ms 时优先补音频，达到约半秒后退回与 LCD 同级；回差防抖。
    return boosted ? buffered_pcm_bytes < 16 * 1024 : buffered_pcm_bytes < 8 * 1024;
}
inline unsigned PlaybackDecodePriority(bool prompt, bool tts = false, bool tts_boost = false) {
    return prompt || (tts && tts_boost) ? 8 : (tts ? 7 : 6);
}
inline int64_t PlaybackDecodeYieldIntervalUs(bool prompt, bool tts_boost = false) {
    return prompt || tts_boost ? 10 * 1000 : 50 * 1000;
}
