#pragma once

#include "core/playback_event.h"
#include "core/playback_request.h"

class PlayerEngine;

class PlayerChannel {
public:
    virtual ~PlayerChannel() = default;

    virtual PlaybackChannel Type() const = 0;
    virtual uint32_t Submit(const PlaybackRequest& request, PlayerEngine& engine) = 0;
    virtual void Cancel(PlayerEngine& engine) = 0;
    virtual void OnEvent(const PlaybackEvent& event) { (void)event; }
};
