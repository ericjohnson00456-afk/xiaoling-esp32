#pragma once

#include <cstdint>

#include "playback_request.h"

enum class PlaybackState {
    // HTTP/TLS 已建连且请求已发出，媒体响应体可能仍在等待云端生成。
    kStreamReady,
    kPlaying,
    kPaused,
    kStopped,
    kCompleted,
    kFailed,
};

enum class PlaybackEndReason {
    kNone,
    kNaturalEnd,
    kPreempted,
    kCancelled,
    kFailed,
    kFirstPcmTimeout,
    kPcmStallTimeout,
};

struct PlaybackEvent {
    uint32_t session_id = 0;
    PlaybackChannel channel = PlaybackChannel::kPrompt;
    PlaybackState state = PlaybackState::kStopped;
    PlaybackEndReason reason = PlaybackEndReason::kNone;
};

// PCM 已经进入物理扬声器输出链路后的事件。
// 与 PlaybackEvent 分开，避免把“解码器开始运行”误认为“扬声器已经出声”。
struct PlaybackOutputEvent {
    uint32_t session_id = 0;
    PlaybackChannel channel = PlaybackChannel::kPrompt;
    uint32_t audio_clock_ms = 0;
};
