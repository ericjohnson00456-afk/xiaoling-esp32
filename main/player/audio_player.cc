#include "audio_player.h"
#include "xiaoling/session/voice_session_epoch.h"

#include "channels/alarm_player.h"
#include "channels/tone_player.h"
#include "channels/tts_player.h"
#include "channels/music_player.h"
#include "backend/esp_simple_player_backend.h"
#include "audio_service.h"
#include "event/app_event.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <utility>

#define TAG "AudioPlayer"

namespace {

AppEvent::Id ToAppEventId(PlaybackState state) {
    switch (state) {
        case PlaybackState::kStreamReady:
            return AppEvent::Id::PlaybackStreamReady;
        case PlaybackState::kPlaying:
            return AppEvent::Id::PlaybackStarted;
        case PlaybackState::kPaused:
            return AppEvent::Id::PlaybackPaused;
        case PlaybackState::kStopped:
            return AppEvent::Id::PlaybackStopped;
        case PlaybackState::kCompleted:
            return AppEvent::Id::PlaybackCompleted;
        case PlaybackState::kFailed:
            return AppEvent::Id::PlaybackFailed;
    }
    return AppEvent::Id::None;
}

}  // namespace

AudioPlayer::AudioPlayer()
    : music_playlist_(std::make_unique<MusicPlaylist>([this](const MusicTrack& track,
                                                              const std::string& location) {
          return track.source == MusicSource::kTfCard
                     ? PlayMusicFileInternal(location)
                     : PlayMusicUrlInternal(location);
      }, [this](MusicTrack track) {
          MusicTrackCallback callback;
          {
              std::lock_guard<std::mutex> lock(callback_mutex_);
              callback = music_track_callback_;
          }
          if (callback) {
              callback(std::move(track));
          }
      })) {}
AudioPlayer::~AudioPlayer() {
    AppEvent::Unsubscribe(backend_event_subscription_id_);
}

bool AudioPlayer::Initialize(AudioService* audio_service, ControlScheduler scheduler) {
    if (backend_ != nullptr) {
        return true;
    }
    if (!scheduler) return false;
    control_scheduler_ = std::move(scheduler);

    if (backend_event_subscription_id_ == 0) {
        backend_event_subscription_id_ = AppEvent::Subscribe(
            AppEvent::Id::PlaybackBackend,
            [](const AppEvent::Event& queued, void* context) {
                const auto* event = queued.Get<PlaybackEvent>();
                if (event == nullptr) {
                    ESP_LOGE(TAG, "播放器内部事件数据无效");
                    return;
                }
                auto* player = static_cast<AudioPlayer*>(context);
                const auto scope = queued.Scope();
                player->control_scheduler_([player, playback = *event, scope]() {
                    if (scope.IsCurrent()) player->HandleBackendEvent(playback);
                });
            },
            this);
        if (backend_event_subscription_id_ == 0) {
            ESP_LOGE(TAG, "订阅播放器内部事件失败");
            return false;
        }
    }

    auto backend = std::make_unique<EspSimplePlayerBackend>();
    if (!backend->Initialize(audio_service, [this](const PlaybackEvent& event) {
            PublishBackendEvent(event);
        })) {
        ESP_LOGE(TAG, "播放器后端初始化失败");
        AppEvent::Unsubscribe(backend_event_subscription_id_);
        backend_event_subscription_id_ = 0;
        return false;
    }
    audio_service_ = audio_service;
    AudioServiceOutputCallbacks output_callbacks;
    output_callbacks.on_output_started = [this](const AudioOutputEvent& event) {
        PublishPlayerOutputStarted(event);
    };
    output_callbacks.on_output_progress = [this](const AudioOutputEvent& event) {
        PublishPlayerOutputProgress(event);
    };
    audio_service_->SetOutputCallbacks(output_callbacks);
    backend_ = std::move(backend);
    tone_channel_ = std::make_unique<TonePlayer>(*backend_, [this]() {
        return next_session_id_.fetch_add(1);
    });
    tts_channel_ = std::make_unique<TtsPlayer>(*backend_, [this]() {
        return next_session_id_.fetch_add(1);
    });
    alarm_channel_ = std::make_unique<AlarmPlayer>(*backend_, [this]() {
        return next_session_id_.fetch_add(1);
    });
    tts_channel_->SetUrlCallback([this]() {
        TtsUrlCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = tts_url_callback_;
        }
        if (callback) {
            callback();
        }
    });
    tts_channel_->SetAdmissionCallback([this]() {
        return !wakeup_in_progress_ && PauseMusic();
    });
    return true;
}

uint32_t AudioPlayer::PlayTtsUrl(const std::string& url) {
    if (backend_ == nullptr || tts_channel_ == nullptr) {
        ESP_LOGE(TAG, "播放器尚未初始化");
        return 0;
    }
    const uint32_t session_id = tts_channel_->PlayUrl(url);
    RememberSession(session_id, PlaybackChannel::kTts);
    return session_id;
}

uint32_t AudioPlayer::PlayMusicUrl(const std::string& url) {
    return music_playlist_->PlayUrl(url);
}

uint32_t AudioPlayer::PlayMusicList(const std::vector<std::string>& urls) {
    return music_playlist_->PlayUrlList(urls);
}

bool AudioPlayer::PlayMusicItemList(const std::vector<MusicTrack>& tracks,
                                    MusicUrlResolver resolver,
                                    std::size_t start_index) {
    return music_playlist_->PlayItemList(tracks, std::move(resolver), start_index);
}

bool AudioPlayer::PlayPreviousMusic(bool play_when_ready) {
    return music_playlist_->PlayPrevious(play_when_ready);
}

bool AudioPlayer::PlayNextMusic(bool play_when_ready) {
    if (music_playlist_->HasList() && music_playlist_->IsAtLast()) {
        // 顺序播放到最后一首后停止当前后端；Application 收到停止事件后弹出 MusicIntent。
        const uint32_t session_id = music_playlist_->CurrentSessionId();
        const bool has_backend = music_backend_ != nullptr;
        const bool stopped = !has_backend ||
                             music_backend_->Stop(PlaybackEndReason::kCancelled);
        music_playlist_->Clear();
        if (!has_backend) {
            // 首个 URL 仍在解析时还没有后端，自然也不会产生停止事件；补发一个
            // 统一的音乐终态，确保音乐 Intent 仍能正常出栈。
            const PlaybackEvent event{
                .session_id = session_id,
                .channel = PlaybackChannel::kMusic,
                .state = PlaybackState::kStopped,
                .reason = PlaybackEndReason::kCancelled,
            };
            if (!AppEvent::Publish(AppEvent::Id::PlaybackStopped, event)) {
                ESP_LOGE(TAG, "发布音乐列表结束事件失败");
            }
        }
        return stopped;
    }
    return music_playlist_->PlayNext(play_when_ready);
}

bool AudioPlayer::ReplayMusic(bool play_when_ready) {
    return music_playlist_->Replay(play_when_ready);
}

void AudioPlayer::ClearMusicList() {
    music_playlist_->Clear();
}

bool AudioPlayer::HasMusicList() const {
    return music_playlist_->HasList();
}

uint32_t AudioPlayer::PlayMusicUrlInternal(const std::string& url) {
    if (backend_ == nullptr || audio_service_ == nullptr || wakeup_in_progress_ ||
        !TtsPlayer::IsHttpUrl(url)) {
        return 0;
    }
    if (music_backend_ == nullptr) {
        auto backend = std::make_unique<EspSimplePlayerBackend>();
        if (!backend->Initialize(audio_service_, [this](const PlaybackEvent& event) {
                PublishBackendEvent(event);
            })) {
            return 0;
        }
        music_backend_ = std::move(backend);
        music_channel_ = std::make_unique<MusicPlayer>(*music_backend_, [this]() {
            return next_session_id_.fetch_add(1);
        });
    }
    if (!StopNonMusic(PlaybackEndReason::kPreempted)) {
        return 0;
    }
    const uint32_t session_id = music_channel_->PlayUrl(url);
    RememberSession(session_id, PlaybackChannel::kMusic);
    return session_id;
}

uint32_t AudioPlayer::PlayMusicFile(const std::string& path) {
    // 串口文件播放是独立调试入口，清除列表，避免播完后触发下一首。
    music_playlist_->Clear();
    return PlayMusicFileInternal(path);
}

uint32_t AudioPlayer::PlayMusicFileInternal(const std::string& path) {
    if (backend_ == nullptr || audio_service_ == nullptr || wakeup_in_progress_ ||
        path.empty()) {
        return 0;
    }
    if (music_backend_ == nullptr) {
        auto backend = std::make_unique<EspSimplePlayerBackend>();
        if (!backend->Initialize(audio_service_, [this](const PlaybackEvent& event) {
                PublishBackendEvent(event);
            })) {
            return 0;
        }
        music_backend_ = std::move(backend);
        music_channel_ = std::make_unique<MusicPlayer>(*music_backend_, [this]() {
            return next_session_id_.fetch_add(1);
        });
    }
    if (!StopNonMusic(PlaybackEndReason::kPreempted)) {
        return 0;
    }
    const uint32_t session_id = music_channel_->PlayFile(path);
    RememberSession(session_id, PlaybackChannel::kMusic);
    return session_id;
}

uint32_t AudioPlayer::PlayTone(uint16_t tone_id) {
    if (backend_ == nullptr || tone_channel_ == nullptr || wakeup_in_progress_) {
        ESP_LOGW(TAG, "播放器不可用或唤醒提示音正在播放");
        return 0;
    }
    if (!PauseMusic()) {
        return 0;
    }
    const uint32_t session_id = tone_channel_->PlayTone(tone_id);
    RememberSession(session_id, PlaybackChannel::kPrompt);
    return session_id;
}

uint32_t AudioPlayer::PlayAlarmTone(uint16_t tone_id) {
    if (backend_ == nullptr || alarm_channel_ == nullptr || wakeup_in_progress_) {
        ESP_LOGW(TAG, "播放器不可用或唤醒提示音正在播放");
        return 0;
    }
    if (!PauseMusic() || !StopNonMusic(PlaybackEndReason::kPreempted)) {
        return 0;
    }
    const uint32_t session_id = alarm_channel_->PlayTone(tone_id);
    RememberSession(session_id, PlaybackChannel::kAlarm);
    return session_id;
}

uint32_t AudioPlayer::PlayAlarmTtsUrl(const std::string& url) {
    if (backend_ == nullptr || alarm_channel_ == nullptr || wakeup_in_progress_) {
        ESP_LOGW(TAG, "播放器不可用或唤醒提示音正在播放");
        return 0;
    }
    if (!PauseMusic() || !StopNonMusic(PlaybackEndReason::kPreempted)) {
        return 0;
    }
    const uint32_t session_id = alarm_channel_->PlayUrl(url);
    RememberSession(session_id, PlaybackChannel::kAlarm);
    return session_id;
}

void AudioPlayer::PlayWakeup(std::function<void(bool)> completion) {
    std::lock_guard<std::recursive_mutex> lock(wakeup_mutex_);
    wakeup_in_progress_ = true;
    if (!PauseMusic() || !StopNonMusic(PlaybackEndReason::kPreempted)) {
        wakeup_in_progress_ = false;
        if (completion) completion(false);
        return;
    }
    if (tone_channel_ == nullptr) {
        wakeup_in_progress_ = false;
        if (completion) completion(true);
        return;
    }
    const auto generation = Xiaoling::voice_session_epoch.Current();
    tone_channel_->PlayWakeup([this, generation, completion = std::move(completion)](bool proceed) {
        std::lock_guard<std::recursive_mutex> lock(wakeup_mutex_);
        if (!Xiaoling::voice_session_epoch.IsCurrent(generation)) return;
        wakeup_in_progress_ = false;
        if (tts_channel_ != nullptr) {
            if (proceed) {
                tts_channel_->ResumeDeferredUrl();
            } else {
                tts_channel_->ClearDeferredUrl();
            }
        }
        if (completion) completion(proceed);
    });
    RememberSession(tone_channel_->WakeupSessionId(), PlaybackChannel::kPrompt);
}

void AudioPlayer::CancelWakeup() {
    std::lock_guard<std::recursive_mutex> lock(wakeup_mutex_);
    if (tone_channel_ != nullptr) {
        tone_channel_->CancelWakeup();
    }
    wakeup_in_progress_ = false;
    if (tts_channel_ != nullptr) {
        tts_channel_->ClearDeferredUrl();
    }
}

bool AudioPlayer::IsWakeupPending() const {
    return tone_channel_ != nullptr && tone_channel_->IsWakeupPending();
}

bool AudioPlayer::Pause() {
    if (backend_ != nullptr && backend_->IsPlaying()) {
        return backend_->Pause();
    }
    return music_channel_ != nullptr && music_channel_->IsAudible() && PauseMusic();
}

bool AudioPlayer::Resume() {
    if (wakeup_in_progress_) {
        return false;
    }
    if (backend_ != nullptr && backend_->IsPlaying()) {
        return backend_->Resume();
    }
    return music_channel_ != nullptr && music_channel_->Resume();
}

bool AudioPlayer::Stop(PlaybackEndReason reason) {
    const bool main_stopped = StopNonMusic(reason);
    const bool music_stopped = music_backend_ == nullptr || music_backend_->Stop(reason);
    if (audio_service_ != nullptr) {
        audio_service_->ClearPlaybackQueue();
    }
    return main_stopped && music_stopped;
}

bool AudioPlayer::StopLocalFilePlayback(PlaybackEndReason reason) {
    if (music_channel_ == nullptr || !music_channel_->IsLocalFilePlaying()) {
        return true;
    }
    if (music_backend_ == nullptr || !music_backend_->Stop(reason)) {
        return false;
    }

    // 拔卡流程必须等文件 IO 真正关闭后才能卸载 FATFS。
    constexpr TickType_t kPollPeriod = pdMS_TO_TICKS(20);
    constexpr int kMaxPolls = 100;
    for (int i = 0; i < kMaxPolls; ++i) {
        if (!music_backend_->IsPlaying()) {
            return true;
        }
        vTaskDelay(kPollPeriod);
    }
    ESP_LOGE(TAG, "等待本地文件播放器停止超时");
    return false;
}

bool AudioPlayer::StopNonMusic(PlaybackEndReason reason) {
    return backend_ == nullptr || backend_->Stop(reason);
}

bool AudioPlayer::IsPlaying() const {
    return (backend_ != nullptr && backend_->IsPlaying()) ||
           (music_channel_ != nullptr && music_channel_->IsAudible());
}

bool AudioPlayer::IsMusicPlaying() const {
    return music_channel_ != nullptr && music_channel_->IsAudible();
}

bool AudioPlayer::ResumeMusic() {
    if (wakeup_in_progress_) {
        return false;
    }
    const uint32_t session_id = music_playlist_->CurrentSessionId();
    const bool resume_paused_session = music_playlist_->HasCurrentSession();
    // Intent 抢占没有修改列表索引时，恢复仍存活的 GMF/HTTP 会话，继续从
    // 暂停点播放；上一首、下一首或重播修改过索引标记时才重新提交 URL。
    if (resume_paused_session && music_channel_ != nullptr) {
        if (music_channel_->IsPaused()) {
            // TTS 已经结束，先恢复暂停点前已经解码的 PCM，再恢复 GMF。
            audio_service_->ResumePlayback(AudioOutputEvent{
                .session_id = session_id,
            });
            if (music_channel_->Resume()) {
                return true;
            }
            audio_service_->ClearPlaybackQueue();
        }
        if (music_channel_->IsAudible()) {
            return true;
        }
    }
    return music_playlist_->PlayCurrent(true);
}

void AudioPlayer::DispatchPlaybackEvent(const PlaybackEvent& event) {
    if (tone_channel_ != nullptr && event.channel == tone_channel_->Type()) {
        tone_channel_->OnEvent(event);
    }
    if (tts_channel_ != nullptr && event.channel == tts_channel_->Type()) {
        tts_channel_->OnEvent(event);
    }
    if (alarm_channel_ != nullptr && event.channel == alarm_channel_->Type()) {
        alarm_channel_->OnEvent(event);
    }
    if (music_channel_ != nullptr && event.channel == music_channel_->Type()) {
        music_channel_->OnEvent(event);
        music_playlist_->HandlePlaybackEvent(event);
    }
}

bool AudioPlayer::PauseMusic() {
    if (music_channel_ == nullptr || !music_channel_->IsAudible()) {
        return true;
    }
    // 保留 GMF 解码器和 HTTP 会话，Intent 恢复时从当前流位置继续播放。
    if (!music_channel_->Pause()) {
        ESP_LOGE(TAG, "停止音乐失败，无法避免覆盖提示音");
        return false;
    }
    // 暂停点前已经解码的 PCM 需要保留，TTS 结束后与 GMF 一起续播。
    audio_service_->SuspendPlayback();
    return true;
}

void AudioPlayer::PlaySound(std::string_view sound) {
    if (audio_service_ != nullptr) {
        audio_service_->PlaySound(sound);
    }
}

void AudioPlayer::ResetPlayback() {
    if (audio_service_ != nullptr) {
        audio_service_->ResetDecoder();
    }
}

void AudioPlayer::SetTtsUrlCallback(TtsUrlCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    tts_url_callback_ = std::move(callback);
}

void AudioPlayer::SetMusicTrackCallback(MusicTrackCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    music_track_callback_ = std::move(callback);
}

void AudioPlayer::PublishPlayerOutputStarted(const AudioOutputEvent& event) {
    const auto output = ToPlaybackOutputEvent(event);
    const bool published = output.channel == PlaybackChannel::kTts
        ? AppEvent::PublishScoped(AppEvent::Id::PlayerOutputStarted, &output, sizeof(output),
              Xiaoling::voice_session_epoch, TtsSessionGeneration(output.session_id))
        : AppEvent::Publish(AppEvent::Id::PlayerOutputStarted, output);
    if (!published && (output.channel != PlaybackChannel::kTts ||
        Xiaoling::voice_session_epoch.IsCurrent(TtsSessionGeneration(output.session_id)))) {
        ESP_LOGE(TAG, "发布播放器首帧输出事件失败");
    }
}

void AudioPlayer::PublishPlayerOutputProgress(const AudioOutputEvent& event) {
    const auto output = ToPlaybackOutputEvent(event);
    {
        std::lock_guard<std::mutex> lock(output_progress_mutex_);
        latest_output_progress_ = output;
    }
    // 播放进度属于高频事件，只保留一个待处理通知，Application 取最新值。
    if (!output_progress_event_pending_.exchange(true) &&
        !AppEvent::Publish(AppEvent::Id::PlayerOutputProgress)) {
        output_progress_event_pending_.store(false);
    }
}

PlaybackOutputEvent AudioPlayer::TakeLatestOutputProgress() {
    std::lock_guard<std::mutex> lock(output_progress_mutex_);
    const auto latest = latest_output_progress_;
    output_progress_event_pending_.store(false);
    return latest;
}

PlaybackOutputEvent AudioPlayer::ToPlaybackOutputEvent(
        const AudioOutputEvent& event) const {
    PlaybackOutputEvent output;
    output.session_id = event.session_id;
    output.audio_clock_ms = event.audio_clock_ms;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        const auto it = session_channels_.find(event.session_id);
        if (it != session_channels_.end()) {
            output.channel = it->second;
        }
    }
    return output;
}

void AudioPlayer::RememberSession(uint32_t session_id, PlaybackChannel channel) {
    if (session_id == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(session_mutex_);
    session_channels_[session_id] = channel;
    if (channel == PlaybackChannel::kTts) {
        tts_session_generations_.emplace(session_id, Xiaoling::voice_session_epoch.Current());
    }
}

uint32_t AudioPlayer::TtsSessionGeneration(uint32_t session_id) const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    const auto it = tts_session_generations_.find(session_id);
    return it == tts_session_generations_.end() ? 0 : it->second;
}

void AudioPlayer::PublishBackendEvent(const PlaybackEvent& event) {
    RememberSession(event.session_id, event.channel);
    const bool published = event.channel == PlaybackChannel::kTts
        ? AppEvent::PublishScoped(AppEvent::Id::PlaybackBackend, &event, sizeof(event),
              Xiaoling::voice_session_epoch, TtsSessionGeneration(event.session_id))
        : AppEvent::Publish(AppEvent::Id::PlaybackBackend, event);
    if (!published && (event.channel != PlaybackChannel::kTts ||
        Xiaoling::voice_session_epoch.IsCurrent(TtsSessionGeneration(event.session_id)))) {
        ESP_LOGE(TAG, "发布播放器内部事件失败: session=%lu, state=%u",
                 static_cast<unsigned long>(event.session_id),
                 static_cast<unsigned>(event.state));
    }
}

void AudioPlayer::HandleBackendEvent(const PlaybackEvent& event) {
    // 此处已经进入 Application 主任务。先维护播放器内部状态，再对外发布
    // 语义明确的生命周期事件，订阅者不需要了解 GMF 回调线程。
    // TonePlayer 的完成回调在 wakeup_mutex_ 下统一恢复暂存 URL；不能在
    // 检查 session 与调用回调之间先无条件恢复，否则旧终态可能影响新唤醒。

    DispatchPlaybackEvent(event);

    const AppEvent::Id id = ToAppEventId(event.state);
    const bool published = id == AppEvent::Id::None ||
        (event.channel == PlaybackChannel::kTts
            ? AppEvent::PublishScoped(id, &event, sizeof(event), Xiaoling::voice_session_epoch,
                                      TtsSessionGeneration(event.session_id))
            : AppEvent::Publish(id, event));
    if (!published && (event.channel != PlaybackChannel::kTts ||
        Xiaoling::voice_session_epoch.IsCurrent(TtsSessionGeneration(event.session_id)))) {
        ESP_LOGE(TAG, "发布播放生命周期事件失败: session=%lu, state=%u",
                 static_cast<unsigned long>(event.session_id),
                 static_cast<unsigned>(event.state));
    }
}
