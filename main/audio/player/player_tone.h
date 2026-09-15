#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "core/playback_event.h"

class AudioPlayer;

class PlayerTone {
public:
    explicit PlayerTone(AudioPlayer& player) : player_(player) {}

    void PlayWakeup(std::function<void(bool)> completion);
    void HandlePlaybackEvent(const PlaybackEvent& event);
    void CancelWakeup();
    bool IsWakeupPending() const { return static_cast<bool>(completion_); }

private:
    std::optional<uint16_t> SelectWakeupTone();
    void CompleteWakeup(bool proceed);

    AudioPlayer& player_;
    std::optional<uint16_t> last_wakeup_tone_;
    uint32_t wakeup_session_id_ = 0;
    std::function<void(bool)> completion_;
};
