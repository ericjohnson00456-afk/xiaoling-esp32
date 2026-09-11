#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "core/playback_event.h"

class AudioService;
class PlayerEngine;

class AudioPlayer {
public:
    using EventCallback = std::function<void(const PlaybackEvent&)>;

    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool Initialize(AudioService* audio_service);
    uint32_t PlayTtsUrl(const std::string& url);
    bool Stop(PlaybackEndReason reason = PlaybackEndReason::kCancelled);
    bool IsPlaying() const;
    void SetEventCallback(EventCallback callback);

private:
    void HandleEngineEvent(const PlaybackEvent& event);

    std::unique_ptr<PlayerEngine> engine_;
    std::atomic<uint32_t> next_session_id_{1};
    mutable std::mutex callback_mutex_;
    EventCallback event_callback_;
};
