#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <cstdint>
#include <vector>

#include "core/playback_event.h"

class AudioService;
class PlayerEngine;
class TonePlayer;
class TtsPlayer;
class MusicPlayer;

class AudioPlayer {
public:
    using EventCallback = std::function<void(const PlaybackEvent&)>;
    using TtsUrlCallback = std::function<void()>;
    using MusicUrlResolveCallback = std::function<void(bool, std::string)>;
    using MusicUrlResolver = std::function<bool(const std::string&, MusicUrlResolveCallback)>;

    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool Initialize(AudioService* audio_service);
    uint32_t PlayTtsUrl(const std::string& url);
    uint32_t PlayMusicUrl(const std::string& url);
    uint32_t PlayMusicList(const std::vector<std::string>& urls);
    bool PlayMusicItemList(const std::vector<std::string>& item_ids,
                           MusicUrlResolver resolver);
    bool PlayPreviousMusic(bool play_when_ready = true);
    bool PlayNextMusic(bool play_when_ready = true);
    bool ReplayMusic(bool play_when_ready = true);
    void ClearMusicList();
    bool HasMusicList() const;
    uint32_t PlayTone(uint16_t tone_id);
    uint32_t PlayAlarmTone(uint16_t tone_id);
    uint32_t PlayAlarmTtsUrl(const std::string& url);
    void PlayWakeup(std::function<void(bool)> completion);
    void CancelWakeup();
    bool IsWakeupPending() const;
    bool Pause();
    bool Resume();
    bool Stop(PlaybackEndReason reason = PlaybackEndReason::kCancelled);
    bool StopNonMusic(PlaybackEndReason reason = PlaybackEndReason::kPreempted);
    bool IsPlaying() const;
    bool IsMusicPlaying() const;
    bool PauseMusic();
    bool ResumeMusic();
    void DispatchPlaybackEvent(const PlaybackEvent& event);
    void SetEventCallback(EventCallback callback);
    void SetTtsUrlCallback(TtsUrlCallback callback);

private:
    struct MusicListItem {
        std::string item_id;
        std::string url;
        bool resolving = false;
        bool play_when_resolved = false;
    };

    void HandleEngineEvent(const PlaybackEvent& event);
    uint32_t PlayMusicUrlInternal(const std::string& url);
    bool RequestMusicAtIndex(std::size_t index, bool play_when_ready);
    void HandleMusicUrlResolved(uint32_t generation, std::size_t index,
                                bool success, std::string url);
    bool StartResolvedMusic(std::size_t index, const std::string& url);
    void PrefetchNextMusic(std::size_t current_index);

    AudioService* audio_service_ = nullptr;
    std::unique_ptr<PlayerEngine> engine_;
    std::unique_ptr<PlayerEngine> music_engine_;
    std::unique_ptr<TonePlayer> tone_channel_;
    std::unique_ptr<TtsPlayer> tts_channel_;
    std::unique_ptr<MusicPlayer> music_channel_;
    mutable std::mutex music_list_mutex_;
    std::vector<MusicListItem> music_list_;
    std::size_t current_music_index_ = 0;
    uint32_t music_list_generation_ = 0;
    uint32_t current_music_session_id_ = 0;
    bool current_music_retry_used_ = false;
    MusicUrlResolver music_url_resolver_;
    std::atomic_bool wakeup_in_progress_{false};
    std::atomic<uint32_t> next_session_id_{1};
    mutable std::mutex callback_mutex_;
    EventCallback event_callback_;
    TtsUrlCallback tts_url_callback_;
};
