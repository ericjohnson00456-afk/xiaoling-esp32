#include "audio_player.h"

#include "channels/tone_player.h"
#include "channels/tts_player.h"
#include "engine/gmf_player_engine.h"

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
    engine_ = std::move(engine);
    tone_channel_ = std::make_unique<TonePlayer>(*engine_, [this]() {
        return next_session_id_.fetch_add(1);
    });
    tts_channel_ = std::make_unique<TtsPlayer>(*engine_, [this]() {
        return next_session_id_.fetch_add(1);
    });
    tts_channel_->SetUrlCallback([this]() {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (tts_url_callback_) {
            tts_url_callback_();
        }
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

uint32_t AudioPlayer::PlayTone(uint16_t tone_id) {
    if (engine_ == nullptr || tone_channel_ == nullptr) {
        ESP_LOGE(TAG, "播放器尚未初始化");
        return 0;
    }
    return tone_channel_->PlayTone(tone_id);
}

void AudioPlayer::PlayWakeup(std::function<void(bool)> completion) {
    if (tone_channel_ != nullptr) {
        tone_channel_->PlayWakeup(std::move(completion));
    } else if (completion) {
        completion(true);
    }
}

void AudioPlayer::CancelWakeup() {
    if (tone_channel_ != nullptr) {
        tone_channel_->CancelWakeup();
    }
}

bool AudioPlayer::IsWakeupPending() const {
    return tone_channel_ != nullptr && tone_channel_->IsWakeupPending();
}

bool AudioPlayer::Pause() {
    return engine_ != nullptr && engine_->Pause();
}

bool AudioPlayer::Resume() {
    return engine_ != nullptr && engine_->Resume();
}

bool AudioPlayer::Stop(PlaybackEndReason reason) {
    return engine_ == nullptr || engine_->Stop(reason);
}

bool AudioPlayer::IsPlaying() const {
    return engine_ != nullptr && engine_->IsPlaying();
}

void AudioPlayer::DispatchPlaybackEvent(const PlaybackEvent& event) {
    if (tone_channel_ != nullptr && event.channel == tone_channel_->Type()) {
        tone_channel_->OnEvent(event);
    }
    if (tts_channel_ != nullptr && event.channel == tts_channel_->Type()) {
        tts_channel_->OnEvent(event);
    }
}

void AudioPlayer::SetEventCallback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_callback_ = std::move(callback);
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
