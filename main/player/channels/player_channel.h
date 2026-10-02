#pragma once

#include "types/playback_event.h"
#include "types/playback_request.h"

class PlaybackBackend;

class PlayerChannel {
public:
    virtual ~PlayerChannel() = default;

    virtual PlaybackChannel Type() const = 0;
    virtual uint32_t Submit(const PlaybackRequest& request, PlaybackBackend& engine) = 0;
    virtual void Cancel(PlaybackBackend& engine) = 0;
    virtual void OnEvent(const PlaybackEvent& event) { (void)event; }
};
