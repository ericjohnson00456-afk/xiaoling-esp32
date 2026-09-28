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
