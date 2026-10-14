#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "audio/audio_output.h"
#include "playlist/music_playlist.h"
#include "types/playback_event.h"
#include "types/music_track.h"

class AudioService;
class PlaybackBackend;
class TonePlayer;
class TtsPlayer;
class MusicPlayer;
class AlarmPlayer;

class AudioPlayer {
public:
    using TtsUrlCallback = std::function<void()>;
    using ControlScheduler = std::function<void(std::function<void()>)>;
    using MusicUrlResolveCallback = MusicPlaylist::MusicUrlResolveCallback;
    using MusicUrlResolver = MusicPlaylist::MusicUrlResolver;
    using MusicTrackCallback = std::function<void(MusicTrack)>;

    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool Initialize(AudioService* audio_service, ControlScheduler scheduler);
    uint32_t PlayTtsUrl(const std::string& url);
    uint32_t PlayMusicUrl(const std::string& url);
    uint32_t PlayMusicList(const std::vector<std::string>& urls);
    bool PlayMusicItemList(const std::vector<MusicTrack>& tracks,
                           MusicUrlResolver resolver,
                           std::size_t start_index = 0);
    bool PlayPreviousMusic(bool play_when_ready = true);
    bool PlayNextMusic(bool play_when_ready = true);
    bool ReplayMusic(bool play_when_ready = true);
    void ClearMusicList();
    bool HasMusicList() const;
    uint32_t PlayMusicFile(const std::string& path);
    uint32_t PlayTone(uint16_t tone_id);
    uint32_t PlayAlarmTone(uint16_t tone_id);
    uint32_t PlayAlarmTtsUrl(const std::string& url);
    void PlayWakeup(std::function<void(bool)> completion);
    void CancelWakeup();
    bool IsWakeupPending() const;
    bool Pause();
    bool Resume();
    bool Stop(PlaybackEndReason reason = PlaybackEndReason::kCancelled);
    bool StopLocalFilePlayback(
        PlaybackEndReason reason = PlaybackEndReason::kCancelled);
    bool StopNonMusic(PlaybackEndReason reason = PlaybackEndReason::kPreempted);
    bool IsPlaying() const;
    bool IsMusicPlaying() const;
    bool PauseMusic();
    bool ResumeMusic();
    // 应用层的本地提示音统一通过播放器提交，避免直接依赖 AudioService。
    void PlaySound(std::string_view sound);
    void ResetPlayback();
    void SetTtsUrlCallback(TtsUrlCallback callback);
    void SetMusicTrackCallback(MusicTrackCallback callback);

    // 返回并清除最近一次合并后的实际扬声器输出进度。
    PlaybackOutputEvent TakeLatestOutputProgress();

private:
    // 后端线程只复制事件；通道状态通过事件队列转交 Application 主任务。
    void PublishBackendEvent(const PlaybackEvent& event);
    void PublishPlayerOutputStarted(const AudioOutputEvent& event);
    void PublishPlayerOutputProgress(const AudioOutputEvent& event);
    PlaybackOutputEvent ToPlaybackOutputEvent(const AudioOutputEvent& event) const;
    void RememberSession(uint32_t session_id, PlaybackChannel channel);
    uint32_t TtsSessionGeneration(uint32_t session_id) const;
    void HandleBackendEvent(const PlaybackEvent& event);
    void DispatchPlaybackEvent(const PlaybackEvent& event);
    uint32_t PlayMusicUrlInternal(const std::string& url);
    uint32_t PlayMusicFileInternal(const std::string& path);

    AudioService* audio_service_ = nullptr;
    ControlScheduler control_scheduler_;
    std::unique_ptr<PlaybackBackend> backend_;
    std::unique_ptr<PlaybackBackend> music_backend_;
    std::unique_ptr<TonePlayer> tone_channel_;
    std::unique_ptr<TtsPlayer> tts_channel_;
    std::unique_ptr<AlarmPlayer> alarm_channel_;
    std::unique_ptr<MusicPlayer> music_channel_;
    std::unique_ptr<MusicPlaylist> music_playlist_;
    std::atomic_bool wakeup_in_progress_{false};
    std::recursive_mutex wakeup_mutex_;
    std::atomic<uint32_t> next_session_id_{1};
    uint32_t backend_event_subscription_id_ = 0;
    mutable std::mutex callback_mutex_;
    TtsUrlCallback tts_url_callback_;
    MusicTrackCallback music_track_callback_;
    mutable std::mutex output_progress_mutex_;
    PlaybackOutputEvent latest_output_progress_;
    std::atomic_bool output_progress_event_pending_{false};
    mutable std::mutex session_mutex_;
    std::unordered_map<uint32_t, PlaybackChannel> session_channels_;
    std::unordered_map<uint32_t, uint32_t> tts_session_generations_;
};
