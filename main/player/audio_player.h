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
class AlarmPlayer;

class AudioPlayer {
public:
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
    void SetTtsUrlCallback(TtsUrlCallback callback);

    // 返回并清除最近一次合并后的实际扬声器输出进度。
    PlaybackOutputEvent TakeLatestOutputProgress();

private:
    struct MusicListItem {
        std::string item_id;
        std::string url;
        bool resolving = false;
        bool play_when_resolved = false;
    };

    // GMF 所在线程只复制事件；通道状态和业务事件都在 app_event 任务处理。
    void PublishEngineEvent(const PlaybackEvent& event);
    void PublishPlayerOutputStarted(const PlaybackOutputEvent& event);
    void PublishPlayerOutputProgress(const PlaybackOutputEvent& event);
    void HandleEngineEvent(const PlaybackEvent& event);
    void DispatchPlaybackEvent(const PlaybackEvent& event);
    bool PlayNextMusicAfterNaturalEnd(uint32_t session_id);
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
    std::unique_ptr<AlarmPlayer> alarm_channel_;
    std::unique_ptr<MusicPlayer> music_channel_;
    mutable std::mutex music_list_mutex_;
    std::vector<MusicListItem> music_list_;
    std::size_t current_music_index_ = 0;
    std::size_t active_music_index_ = static_cast<std::size_t>(-1);
    uint32_t music_list_generation_ = 0;
    uint32_t current_music_session_id_ = 0;
    bool current_music_retry_used_ = false;
    MusicUrlResolver music_url_resolver_;
    std::atomic_bool wakeup_in_progress_{false};
    std::atomic<uint32_t> next_session_id_{1};
    uint32_t engine_event_subscription_id_ = 0;
    mutable std::mutex callback_mutex_;
    TtsUrlCallback tts_url_callback_;
    mutable std::mutex output_progress_mutex_;
    PlaybackOutputEvent latest_output_progress_;
    std::atomic_bool output_progress_event_pending_{false};
};
