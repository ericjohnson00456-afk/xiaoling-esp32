#include "tts_player.h"

#include "backend/playback_backend.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <utility>

#define TAG "TtsPlayer"

TtsPlayer::TtsPlayer(PlaybackBackend& engine, SessionAllocator allocate_session)
    : engine_(&engine), allocate_session_(std::move(allocate_session)) {}

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

uint32_t TtsPlayer::Submit(const PlaybackRequest& request, PlaybackBackend& engine) {
    return engine.Play(request) ? request.session_id : 0;
}

uint32_t TtsPlayer::PlayUrl(const std::string& url) {
    return PlayUrlInternal(url, true);
}

uint32_t TtsPlayer::PlayUrlInternal(const std::string& url, bool notify) {
    if (!IsHttpUrl(url)) {
        ESP_LOGE(TAG, "拒绝无效的 TTS URL, bytes=%u", static_cast<unsigned>(url.size()));
        return 0;
    }
    if (admission_callback_ && !admission_callback_()) {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        deferred_url_ = url;
        ESP_LOGI(TAG, "唤醒提示音尚未排空，暂存 TTS URL");
        return 0;
    }
    return SubmitUrl(url, notify);
}

uint32_t TtsPlayer::SubmitUrl(const std::string& url, bool notify) {
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

bool TtsPlayer::ResumeDeferredUrl() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        if (deferred_url_.empty()) {
            return false;
        }
        url = std::move(deferred_url_);
        deferred_url_.clear();
    }

    if (admission_callback_ && !admission_callback_()) {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        if (deferred_url_.empty()) {
            deferred_url_ = std::move(url);
        }
        return false;
    }
    if (SubmitUrl(url, true) == 0) {
        ESP_LOGE(TAG, "恢复暂存的 TTS URL 失败");
        return false;
    }
    ESP_LOGI(TAG, "唤醒提示音已排空，提交暂存的 TTS URL");
    return true;
}

void TtsPlayer::ClearDeferredUrl() {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_url_.clear();
}

void TtsPlayer::SetUrlCallback(UrlCallback callback) {
    url_callback_ = std::move(callback);
}

void TtsPlayer::SetAdmissionCallback(AdmissionCallback callback) {
    admission_callback_ = std::move(callback);
}

void TtsPlayer::Cancel(PlaybackBackend& engine) {
    ClearDeferredUrl();
    engine.Stop(PlaybackEndReason::kCancelled);
}
