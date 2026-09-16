#include "tts_player.h"

#include "engine/player_engine.h"
#include "xiaoling/xiaoling_events.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <utility>

#define TAG "TtsPlayer"

TtsPlayer::TtsPlayer(PlayerEngine& engine, SessionAllocator allocate_session)
    : engine_(&engine), allocate_session_(std::move(allocate_session)) {
    subscription_id_ = XiaolingEventBus::GetInstance().Subscribe(
        XIAOLING_TTS_URL, [this](const XiaolingEvent& event) {
            if (PlayUrlInternal(event.text, true) == 0) {
                ESP_LOGE(TAG, "订阅到 TTS URL，但提交流式播放失败");
            }
        });
}

TtsPlayer::~TtsPlayer() {
    XiaolingEventBus::GetInstance().Unsubscribe(subscription_id_);
}

bool TtsPlayer::IsHttpUrl(const std::string& url) {
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

uint32_t TtsPlayer::Submit(const PlaybackRequest& request, PlayerEngine& engine) {
    return engine.Play(request) ? request.session_id : 0;
}

uint32_t TtsPlayer::PlayUrl(const std::string& url) {
    return PlayUrlInternal(url, false);
}

uint32_t TtsPlayer::PlayUrlInternal(const std::string& url, bool notify) {
    if (!IsHttpUrl(url)) {
        ESP_LOGE(TAG, "拒绝无效的 TTS URL, bytes=%u", static_cast<unsigned>(url.size()));
        return 0;
    }
    if (notify && url_callback_) {
        url_callback_();
    }
    PlaybackRequest request;
    request.session_id = allocate_session_();
    request.source_type = PlaybackSourceType::kUrl;
    request.channel = PlaybackChannel::kTts;
    request.source = url;
    request.priority = 50;
    return Submit(request, *engine_);
}

void TtsPlayer::SetUrlCallback(UrlCallback callback) {
    url_callback_ = std::move(callback);
}

void TtsPlayer::Cancel(PlayerEngine& engine) {
    engine.Stop(PlaybackEndReason::kCancelled);
}
