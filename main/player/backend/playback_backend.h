#pragma once

#include <functional>

#include "playback_event.h"
#include "audio/audio_output.h"

class PlaybackBackend {
public:
    using EventCallback = std::function<void(const PlaybackEvent&)>;

    virtual ~PlaybackBackend() = default;

    virtual bool Initialize(AudioOutput* audio_output, EventCallback callback) = 0;
    virtual bool Play(const PlaybackRequest& request) = 0;
    virtual bool Pause() = 0;
    virtual bool Resume() = 0;
    virtual bool Stop(PlaybackEndReason reason) = 0;
    virtual bool IsPlaying() const = 0;
};
