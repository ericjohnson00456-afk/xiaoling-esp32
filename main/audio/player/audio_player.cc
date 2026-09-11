#include "audio_player.h"

#include "engine/gmf_player_engine.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <utility>

#define TAG "AudioPlayer"

namespace {

bool IsHttpUrl(const std::string& url) {
    const size_t scheme_size = url.rfind("https://", 0) == 0 ? 8 :
                               (url.rfind("http://", 0) == 0 ? 7 : 0);
    if (scheme_size == 0 || scheme_size >= url.size()) {
        return false;
    }

    const auto authority_end = url.find_first_of("/?#", scheme_size);
    const auto authority_size = (authority_end == std::string::npos ? url.size() : authority_end) - scheme_size;
    if (authority_size == 0) {
        return false;
    }

    return std::none_of(url.begin(), url.end(), [](unsigned char ch) {
        return std::iscntrl(ch) != 0 || std::isspace(ch) != 0;
    });
}

}  // namespace

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
    return true;
}

uint32_t AudioPlayer::PlayTtsUrl(const std::string& url) {
    if (engine_ == nullptr) {
        ESP_LOGE(TAG, "播放器尚未初始化");
        return 0;
    }
    if (!IsHttpUrl(url)) {
        ESP_LOGE(TAG, "拒绝无效的 TTS URL, bytes=%u", static_cast<unsigned>(url.size()));
        return 0;
    }

    PlaybackRequest request;
    request.session_id = next_session_id_.fetch_add(1);
    request.source_type = PlaybackSourceType::kUrl;
    request.channel = PlaybackChannel::kTts;
    request.source = url;
    request.priority = 50;
    request.resumable = false;
    request.loop = false;

    if (!engine_->Play(request)) {
        return 0;
    }
    return request.session_id;
}

bool AudioPlayer::Stop(PlaybackEndReason reason) {
    return engine_ == nullptr || engine_->Stop(reason);
}

bool AudioPlayer::IsPlaying() const {
    return engine_ != nullptr && engine_->IsPlaying();
}

void AudioPlayer::SetEventCallback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_callback_ = std::move(callback);
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
