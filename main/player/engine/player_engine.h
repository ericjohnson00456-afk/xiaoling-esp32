#pragma once

#include <functional>

#include "playback_event.h"

class AudioService;

class PlayerEngine {
public:
    using EventCallback = std::function<void(const PlaybackEvent&)>;

    virtual ~PlayerEngine() = default;

    virtual bool Initialize(AudioService* audio_service, EventCallback callback) = 0;
    virtual bool Play(const PlaybackRequest& request) = 0;
    virtual bool Pause() = 0;
    virtual bool Resume() = 0;
    virtual bool Stop(PlaybackEndReason reason) = 0;
    virtual bool IsPlaying() const = 0;
};
