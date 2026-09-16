#pragma once

#include <cstdint>

#include "playback_request.h"

enum class PlaybackState {
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
};

struct PlaybackEvent {
    uint32_t session_id = 0;
    PlaybackChannel channel = PlaybackChannel::kPrompt;
    PlaybackState state = PlaybackState::kStopped;
    PlaybackEndReason reason = PlaybackEndReason::kNone;
};
