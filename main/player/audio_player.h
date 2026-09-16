#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <cstdint>

#include "core/playback_event.h"

class AudioService;
class PlayerEngine;
class TonePlayer;
class TtsPlayer;

class AudioPlayer {
public:
    using EventCallback = std::function<void(const PlaybackEvent&)>;
    using TtsUrlCallback = std::function<void()>;

    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool Initialize(AudioService* audio_service);
    uint32_t PlayTtsUrl(const std::string& url);
    uint32_t PlayTone(uint16_t tone_id);
    void PlayWakeup(std::function<void(bool)> completion);
    void CancelWakeup();
    bool IsWakeupPending() const;
    bool Pause();
    bool Resume();
    bool Stop(PlaybackEndReason reason = PlaybackEndReason::kCancelled);
    bool IsPlaying() const;
    void DispatchPlaybackEvent(const PlaybackEvent& event);
    void SetEventCallback(EventCallback callback);
    void SetTtsUrlCallback(TtsUrlCallback callback);

private:
    void HandleEngineEvent(const PlaybackEvent& event);

    std::unique_ptr<PlayerEngine> engine_;
    std::unique_ptr<TonePlayer> tone_channel_;
    std::unique_ptr<TtsPlayer> tts_channel_;
    std::atomic<uint32_t> next_session_id_{1};
    mutable std::mutex callback_mutex_;
    EventCallback event_callback_;
    TtsUrlCallback tts_url_callback_;
};
