#include "audio_player.h"

#include "channels/alarm_player.h"
#include "channels/tone_player.h"
#include "channels/tts_player.h"
#include "channels/music_player.h"
#include "engine/gmf_player_engine.h"
#include "audio_service.h"

#include <esp_log.h>

#include <utility>

#define TAG "AudioPlayer"

AudioPlayer::AudioPlayer() = default;
AudioPlayer::~AudioPlayer() = default;

bool AudioPlayer::Initialize(AudioService* audio_service) {
    if (engine_ != nullptr) {
        return true;
    }

    auto engine = std::make_unique<GmfPlayerEngine>();
    if (!engine->Initialize(audio_service, [this](const PlaybackEvent& event) {
            HandleEngineEvent(event);
        })) {
        ESP_LOGE(TAG, "播放器引擎初始化失败");
        return false;
    }
    audio_service_ = audio_service;
    engine_ = std::move(engine);
    tone_channel_ = std::make_unique<TonePlayer>(*engine_, [this]() {
        return next_session_id_.fetch_add(1);
    });
    tts_channel_ = std::make_unique<TtsPlayer>(*engine_, [this]() {
        return next_session_id_.fetch_add(1);
    });
    alarm_channel_ = std::make_unique<AlarmPlayer>(*engine_, [this]() {
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
    if (engine_ == nullptr || tts_channel_ == nullptr) {
        ESP_LOGE(TAG, "播放器尚未初始化");
        return 0;
    }
    return tts_channel_->PlayUrl(url);
}

uint32_t AudioPlayer::PlayMusicUrl(const std::string& url) {
    if (!TtsPlayer::IsHttpUrl(url)) {
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        music_list_.clear();
        music_list_.push_back(MusicListItem{.url = url});
        current_music_index_ = 0;
        active_music_index_ = static_cast<std::size_t>(-1);
        ++music_list_generation_;
        current_music_session_id_ = 0;
        current_music_retry_used_ = false;
        music_url_resolver_ = nullptr;
    }
    if (!StartResolvedMusic(0, url)) {
        ClearMusicList();
        return 0;
    }
    std::lock_guard<std::mutex> lock(music_list_mutex_);
    return current_music_session_id_;
}

uint32_t AudioPlayer::PlayMusicList(const std::vector<std::string>& urls) {
    std::vector<MusicListItem> valid_urls;
    valid_urls.reserve(urls.size());
    for (const auto& url : urls) {
        if (TtsPlayer::IsHttpUrl(url)) {
            valid_urls.push_back(MusicListItem{.url = url});
        }
    }
    if (valid_urls.empty()) {
        return 0;
    }
    std::string first_url;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        music_list_ = std::move(valid_urls);
        current_music_index_ = 0;
        active_music_index_ = static_cast<std::size_t>(-1);
        first_url = music_list_.front().url;
        ++music_list_generation_;
        current_music_session_id_ = 0;
        current_music_retry_used_ = false;
        music_url_resolver_ = nullptr;
    }
    if (!StartResolvedMusic(0, first_url)) {
        ClearMusicList();
        return 0;
    }
    std::lock_guard<std::mutex> lock(music_list_mutex_);
    return current_music_session_id_;
}

bool AudioPlayer::PlayMusicItemList(const std::vector<std::string>& item_ids,
                                    MusicUrlResolver resolver) {
    if (!resolver) {
        return false;
    }
    std::vector<MusicListItem> items;
    items.reserve(item_ids.size());
    for (const auto& item_id : item_ids) {
        if (!item_id.empty()) {
            items.push_back(MusicListItem{.item_id = item_id});
        }
    }
    if (items.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        music_list_ = std::move(items);
        current_music_index_ = 0;
        active_music_index_ = static_cast<std::size_t>(-1);
        ++music_list_generation_;
        current_music_session_id_ = 0;
        current_music_retry_used_ = false;
        music_url_resolver_ = std::move(resolver);
    }
    if (!RequestMusicAtIndex(0, true)) {
        ClearMusicList();
        return false;
    }
    return true;
}

bool AudioPlayer::PlayPreviousMusic(bool play_when_ready) {
    std::size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (music_list_.empty()) {
            return false;
        }
        current_music_index_ = current_music_index_ == 0
                                   ? music_list_.size() - 1
                                   : current_music_index_ - 1;
        active_music_index_ = static_cast<std::size_t>(-1);
        index = current_music_index_;
        current_music_retry_used_ = false;
    }
    return RequestMusicAtIndex(index, play_when_ready);
}

bool AudioPlayer::PlayNextMusic(bool play_when_ready) {
    std::size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (music_list_.empty()) {
            return false;
        }
        current_music_index_ = (current_music_index_ + 1) % music_list_.size();
        active_music_index_ = static_cast<std::size_t>(-1);
        index = current_music_index_;
        current_music_retry_used_ = false;
    }
    return RequestMusicAtIndex(index, play_when_ready);
}

bool AudioPlayer::ReplayMusic(bool play_when_ready) {
    std::size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (music_list_.empty()) {
            return false;
        }
        index = current_music_index_;
        active_music_index_ = static_cast<std::size_t>(-1);
        current_music_retry_used_ = false;
    }
    return RequestMusicAtIndex(index, play_when_ready);
}

void AudioPlayer::ClearMusicList() {
    std::lock_guard<std::mutex> lock(music_list_mutex_);
    music_list_.clear();
    current_music_index_ = 0;
    active_music_index_ = static_cast<std::size_t>(-1);
    ++music_list_generation_;
    current_music_session_id_ = 0;
    current_music_retry_used_ = false;
    music_url_resolver_ = nullptr;
}

bool AudioPlayer::HasMusicList() const {
    std::lock_guard<std::mutex> lock(music_list_mutex_);
    return !music_list_.empty();
}

bool AudioPlayer::RequestMusicAtIndex(std::size_t index, bool play_when_ready) {
    MusicUrlResolver resolver;
    std::string item_id;
    std::string cached_url;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (index >= music_list_.size()) {
            return false;
        }
        auto& item = music_list_[index];
        if (!item.url.empty()) {
            cached_url = item.url;
        } else {
            if (!music_url_resolver_ || item.item_id.empty()) {
                return false;
            }
            item.play_when_resolved = item.play_when_resolved || play_when_ready;
            if (item.resolving) {
                return true;
            }
            item.resolving = true;
            resolver = music_url_resolver_;
            item_id = item.item_id;
            generation = music_list_generation_;
        }
    }

    if (!cached_url.empty()) {
        return !play_when_ready || StartResolvedMusic(index, cached_url);
    }

    const bool submitted = resolver(
        item_id, [this, generation, index](bool success, std::string url) mutable {
            HandleMusicUrlResolved(generation, index, success, std::move(url));
        });
    if (!submitted) {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (generation == music_list_generation_ && index < music_list_.size()) {
            music_list_[index].resolving = false;
            music_list_[index].play_when_resolved = false;
        }
    }
    return submitted;
}

void AudioPlayer::HandleMusicUrlResolved(uint32_t generation, std::size_t index,
                                         bool success, std::string url) {
    const bool valid_url = success && TtsPlayer::IsHttpUrl(url);
    bool should_play = false;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (generation != music_list_generation_ || index >= music_list_.size()) {
            return;
        }
        auto& item = music_list_[index];
        item.resolving = false;
        if (valid_url) {
            item.url = url;
        }
        should_play = valid_url && item.play_when_resolved &&
                      current_music_index_ == index;
        item.play_when_resolved = false;
    }
    if (should_play) {
        if (!StartResolvedMusic(index, url)) {
            ESP_LOGW(TAG, "解析后的音乐 URL 提交播放失败");
        }
    } else if (!valid_url) {
        ESP_LOGW(TAG, "音乐条目 URL 解析失败: index=%u", static_cast<unsigned>(index));
    }
}

bool AudioPlayer::StartResolvedMusic(std::size_t index, const std::string& url) {
    const uint32_t session_id = PlayMusicUrlInternal(url);
    if (session_id == 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (index >= music_list_.size() || current_music_index_ != index) {
            return false;
        }
        current_music_session_id_ = session_id;
        active_music_index_ = index;
    }
    PrefetchNextMusic(index);
    return true;
}

void AudioPlayer::PrefetchNextMusic(std::size_t current_index) {
    std::size_t next_index = 0;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (music_list_.size() < 2 || current_index >= music_list_.size()) {
            return;
        }
        next_index = (current_index + 1) % music_list_.size();
    }
    RequestMusicAtIndex(next_index, false);
}

uint32_t AudioPlayer::PlayMusicUrlInternal(const std::string& url) {
    if (engine_ == nullptr || audio_service_ == nullptr || wakeup_in_progress_ ||
        !TtsPlayer::IsHttpUrl(url)) {
        return 0;
    }
    if (music_engine_ == nullptr) {
        auto engine = std::make_unique<GmfPlayerEngine>();
        if (!engine->Initialize(audio_service_, [this](const PlaybackEvent& event) {
                HandleEngineEvent(event);
            })) {
            return 0;
        }
        music_engine_ = std::move(engine);
        music_channel_ = std::make_unique<MusicPlayer>(*music_engine_, [this]() {
            return next_session_id_.fetch_add(1);
        });
    }
    if (!StopNonMusic(PlaybackEndReason::kPreempted)) {
        return 0;
    }
    return music_channel_->PlayUrl(url);
}

uint32_t AudioPlayer::PlayTone(uint16_t tone_id) {
    if (engine_ == nullptr || tone_channel_ == nullptr || wakeup_in_progress_) {
        ESP_LOGW(TAG, "播放器不可用或唤醒提示音正在播放");
        return 0;
    }
    if (!PauseMusic()) {
        return 0;
    }
    return tone_channel_->PlayTone(tone_id);
}

uint32_t AudioPlayer::PlayAlarmTone(uint16_t tone_id) {
    if (engine_ == nullptr || alarm_channel_ == nullptr || wakeup_in_progress_) {
        ESP_LOGW(TAG, "播放器不可用或唤醒提示音正在播放");
        return 0;
    }
    if (!PauseMusic() || !StopNonMusic(PlaybackEndReason::kPreempted)) {
        return 0;
    }
    return alarm_channel_->PlayTone(tone_id);
}

uint32_t AudioPlayer::PlayAlarmTtsUrl(const std::string& url) {
    if (engine_ == nullptr || alarm_channel_ == nullptr || wakeup_in_progress_) {
        ESP_LOGW(TAG, "播放器不可用或唤醒提示音正在播放");
        return 0;
    }
    if (!PauseMusic() || !StopNonMusic(PlaybackEndReason::kPreempted)) {
        return 0;
    }
    return alarm_channel_->PlayUrl(url);
}

void AudioPlayer::PlayWakeup(std::function<void(bool)> completion) {
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
    tone_channel_->PlayWakeup([this, completion = std::move(completion)](bool proceed) {
        wakeup_in_progress_ = false;
        if (completion) completion(proceed);
    });
}

void AudioPlayer::CancelWakeup() {
    if (tone_channel_ != nullptr) {
        tone_channel_->CancelWakeup();
    }
    wakeup_in_progress_ = false;
}

bool AudioPlayer::IsWakeupPending() const {
    return tone_channel_ != nullptr && tone_channel_->IsWakeupPending();
}

bool AudioPlayer::Pause() {
    if (engine_ != nullptr && engine_->IsPlaying()) {
        return engine_->Pause();
    }
    return music_channel_ != nullptr && music_channel_->IsAudible() && PauseMusic();
}

bool AudioPlayer::Resume() {
    if (wakeup_in_progress_) {
        return false;
    }
    if (engine_ != nullptr && engine_->IsPlaying()) {
        return engine_->Resume();
    }
    return music_channel_ != nullptr && music_channel_->Resume();
}

bool AudioPlayer::Stop(PlaybackEndReason reason) {
    const bool main_stopped = StopNonMusic(reason);
    const bool music_stopped = music_engine_ == nullptr || music_engine_->Stop(reason);
    return main_stopped && music_stopped;
}

bool AudioPlayer::StopNonMusic(PlaybackEndReason reason) {
    return engine_ == nullptr || engine_->Stop(reason);
}

bool AudioPlayer::IsPlaying() const {
    return (engine_ != nullptr && engine_->IsPlaying()) ||
           (music_channel_ != nullptr && music_channel_->IsAudible());
}

bool AudioPlayer::IsMusicPlaying() const {
    return music_channel_ != nullptr && music_channel_->IsAudible();
}

bool AudioPlayer::ResumeMusic() {
    if (wakeup_in_progress_) {
        return false;
    }
    std::size_t index = 0;
    bool resume_paused_session = false;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (music_list_.empty()) {
            return false;
        }
        index = current_music_index_;
        resume_paused_session = active_music_index_ == current_music_index_ &&
                                current_music_session_id_ != 0;
    }
    // Intent 抢占没有修改列表索引时，恢复仍存活的 GMF/HTTP 会话，继续从
    // 暂停点播放；上一首、下一首或重播修改过索引标记时才重新提交 URL。
    if (resume_paused_session && music_channel_ != nullptr) {
        if (music_channel_->IsPaused()) {
            // TTS 已经结束并关闭了共享 PCM 流，恢复音乐前重新初始化输出状态。
            audio_service_->BeginPlayerPlayback();
            if (music_channel_->Resume()) {
                return true;
            }
            audio_service_->ClearPlaybackQueue();
        }
        if (music_channel_->IsAudible()) {
            return true;
        }
    }
    return RequestMusicAtIndex(index, true);
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
        if (event.state == PlaybackState::kFailed) {
            std::size_t retry_index = 0;
            bool should_retry = false;
            {
                std::lock_guard<std::mutex> lock(music_list_mutex_);
                if (event.session_id == current_music_session_id_ &&
                    current_music_index_ < music_list_.size() &&
                    !music_list_[current_music_index_].item_id.empty() &&
                    !current_music_retry_used_) {
                    retry_index = current_music_index_;
                    auto& item = music_list_[retry_index];
                    item.url.clear();
                    item.resolving = false;
                    item.play_when_resolved = false;
                    current_music_retry_used_ = true;
                    should_retry = true;
                }
            }
            if (should_retry) {
                ESP_LOGW(TAG, "音乐流播放失败，重新解析当前条目 URL");
                RequestMusicAtIndex(retry_index, true);
            }
        }
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
    audio_service_->ClearPlaybackQueue();
    return true;
}

void AudioPlayer::SetEventCallback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_callback_ = std::move(callback);
}

void AudioPlayer::SetAlarmEventCallback(EventCallback callback) {
    if (alarm_channel_ != nullptr) {
        alarm_channel_->SetEventCallback(std::move(callback));
    }
}

void AudioPlayer::SetTtsUrlCallback(TtsUrlCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    tts_url_callback_ = std::move(callback);
}

void AudioPlayer::HandleEngineEvent(const PlaybackEvent& event) {
    EventCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = event_callback_;
    }
    if (callback) {
        callback(event);
    }
}
