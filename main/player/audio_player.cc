#include "audio_player.h"

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
        music_list_.push_back(url);
        current_music_index_ = 0;
    }
    const auto session_id = PlayMusicUrlInternal(url);
    if (session_id != 0) {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = music_start_callback_;
        }
        if (callback) callback();
    }
    return session_id;
}

uint32_t AudioPlayer::PlayMusicList(const std::vector<std::string>& urls) {
    std::vector<std::string> valid_urls;
    valid_urls.reserve(urls.size());
    for (const auto& url : urls) {
        if (TtsPlayer::IsHttpUrl(url)) {
            valid_urls.push_back(url);
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
        first_url = music_list_.front();
    }
    const auto session_id = PlayMusicUrlInternal(first_url);
    if (session_id != 0) {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = music_start_callback_;
        }
        if (callback) callback();
    }
    return session_id;
}

bool AudioPlayer::PlayPreviousMusic() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (music_list_.empty()) {
            return false;
        }
        current_music_index_ = current_music_index_ == 0
                                   ? music_list_.size() - 1
                                   : current_music_index_ - 1;
        url = music_list_[current_music_index_];
    }
    return PlayMusicUrlInternal(url) != 0;
}

bool AudioPlayer::PlayNextMusic() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (music_list_.empty()) {
            return false;
        }
        current_music_index_ = (current_music_index_ + 1) % music_list_.size();
        url = music_list_[current_music_index_];
    }
    return PlayMusicUrlInternal(url) != 0;
}

bool AudioPlayer::ReplayMusic() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(music_list_mutex_);
        if (music_list_.empty()) {
            return false;
        }
        url = music_list_[current_music_index_];
    }
    return PlayMusicUrlInternal(url) != 0;
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
    if (wakeup_in_progress_ || music_channel_ == nullptr) {
        return false;
    }
    return music_channel_->Resume();
}

void AudioPlayer::DispatchPlaybackEvent(const PlaybackEvent& event) {
    if (tone_channel_ != nullptr && event.channel == tone_channel_->Type()) {
        tone_channel_->OnEvent(event);
    }
    if (tts_channel_ != nullptr && event.channel == tts_channel_->Type()) {
        tts_channel_->OnEvent(event);
    }
    if (music_channel_ != nullptr && event.channel == music_channel_->Type()) {
        music_channel_->OnEvent(event);
    }
}

bool AudioPlayer::PauseMusic() {
    if (music_channel_ == nullptr || !music_channel_->IsAudible()) {
        return true;
    }
    // HTTP streaming cannot be paused reliably because the GMF worker may be
    // blocked in the network reader. Stop it immediately so wakeup is not
    // delayed by the HTTP timeout; MusicPlayer can restart the URL on resume.
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

void AudioPlayer::SetTtsUrlCallback(TtsUrlCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    tts_url_callback_ = std::move(callback);
}

void AudioPlayer::SetMusicStartCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    music_start_callback_ = std::move(callback);
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
