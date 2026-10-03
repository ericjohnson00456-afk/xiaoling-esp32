#include "voice_subtitle_controller.h"

#include "event/app_event.h"
#include "home_presenter.h"
#include "home_view.h"
#include "ui/i18n/lang_config.h"
#include "ui/ui_manager.h"
#include "system/device_status.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "VoiceSubtitle"

namespace {

bool ReadJsonUint32(const cJSON* object, const char* name, uint32_t* value) {
    if (!cJSON_IsObject(object) || value == nullptr) {
        return false;
    }
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > static_cast<double>(UINT32_MAX)) {
        return false;
    }
    const auto converted = static_cast<uint32_t>(item->valuedouble);
    if (static_cast<double>(converted) != item->valuedouble) {
        return false;
    }
    *value = converted;
    return true;
}

}  // namespace

VoiceSubtitleController::VoiceSubtitleController(HomePresenter& presenter)
    : presenter_(presenter) {
}

void VoiceSubtitleController::Start() {
    if (!event_subscriptions_.empty()) {
        return;
    }
    auto subscribe_text = [this](AppEvent::Id id) {
        event_subscriptions_.push_back(AppEvent::Subscribe(
            id, [this](const AppEvent::Event& event) {
                QueueIatText(std::string(event.Text()));
            }));
    };
    subscribe_text(AppEvent::Id::XiaolingIatStart);
    subscribe_text(AppEvent::Id::XiaolingIatUpdate);
    subscribe_text(AppEvent::Id::XiaolingIatEnd);
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsTextStart, [this](const AppEvent::Event&) {
            UiManager::GetInstance().Dispatch([this]() { OnTtsTextStart(); });
        }));
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsTextUpdate, [this](const AppEvent::Event& event) {
            UiManager::GetInstance().Dispatch(
                [this, text = std::string(event.Text())]() { OnTtsTextUpdate(text); });
        }));
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsTextInterrupt, [this](const AppEvent::Event&) {
            UiManager::GetInstance().Dispatch([this]() { OnTtsTextInterrupt(); });
        }));
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsStarted, [this](const AppEvent::Event&) {
            const uint32_t generation = tts_progress_generation_.fetch_add(1) + 1;
            latest_tts_audio_clock_ms_.store(0);
            tts_progress_dispatch_pending_.store(false);
            UiManager::GetInstance().Dispatch([this, generation]() {
                if (generation == tts_progress_generation_.load()) {
                    OnTtsPlaying();
                }
            });
        }));
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsOutputProgress,
        [this](const AppEvent::Event& event) {
            uint32_t audio_clock_ms = 0;
            const std::string text(event.Text());
            if (!text.empty()) {
                char* end = nullptr;
                const unsigned long value = std::strtoul(text.c_str(), &end, 10);
                if (end != text.c_str() && *end == '\0' && value <= UINT32_MAX) {
                    audio_clock_ms = static_cast<uint32_t>(value);
                }
            }
            QueueTtsAudioProgress(audio_clock_ms);
        }));
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsStopped, [this](const AppEvent::Event&) {
            const uint32_t generation = tts_progress_generation_.fetch_add(1) + 1;
            latest_tts_audio_clock_ms_.store(0);
            tts_progress_dispatch_pending_.store(false);
            UiManager::GetInstance().Dispatch([this, generation]() {
                if (generation == tts_progress_generation_.load()) {
                    OnTtsStopped();
                }
            });
        }));
}

void VoiceSubtitleController::Stop() {
    for (const auto id : event_subscriptions_) {
        AppEvent::Unsubscribe(id);
    }
    event_subscriptions_.clear();
    ResetSubtitles(true);
    {
        std::lock_guard<std::mutex> lock(iat_text_mutex_);
        pending_iat_text_.clear();
        pending_iat_received_us_ = 0;
        iat_dispatch_pending_ = false;
        ++last_voice_text_revision_;
    }
    iat_text_visible_ = false;
    tts_progress_generation_.fetch_add(1);
    latest_tts_audio_clock_ms_.store(0);
    tts_progress_dispatch_pending_.store(false);
}

void VoiceSubtitleController::QueueIatText(std::string text) {
    if (text.empty()) return;
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(iat_text_mutex_);
        pending_iat_text_ = std::move(text);
        pending_iat_received_us_ = esp_timer_get_time();
        ++last_voice_text_revision_;
        if (!iat_dispatch_pending_) {
            iat_dispatch_pending_ = true;
            schedule = true;
        }
    }
    if (schedule) {
        UiManager::GetInstance().Dispatch([this]() { DrainIatText(); });
    }
}

void VoiceSubtitleController::DrainIatText() {
    std::string text;
    uint32_t revision = 0;
    int64_t received_us = 0;
    {
        std::lock_guard<std::mutex> lock(iat_text_mutex_);
        text = pending_iat_text_;
        revision = last_voice_text_revision_;
        received_us = pending_iat_received_us_;
    }
    if (!text.empty()) {
        if (subtitle_audio_started_) {
            ResetSubtitles(true);
            presenter_.SetStatus(Lang::Strings::LISTENING);
            presenter_.SetEmotion("聆听");
            ESP_LOGI(TAG, "非空 IAT 已接管 TTS UI，字幕时钟停止");
        }
        iat_text_visible_ = true;
        presenter_.SetText(std::move(text));
        const int64_t now_us = esp_timer_get_time();
        const int64_t delay_ms = (now_us - received_us) / 1000;
        if (delay_ms >= 100 && now_us - last_iat_delay_log_us_ >= 1000000) {
            last_iat_delay_log_us_ = now_us;
            ESP_LOGW(TAG, "IAT UI 更新延迟: %ldms", static_cast<long>(delay_ms));
        }
    }
    bool reschedule = false;
    {
        std::lock_guard<std::mutex> lock(iat_text_mutex_);
        if (revision == last_voice_text_revision_) {
            iat_dispatch_pending_ = false;
        } else {
            reschedule = true;
        }
    }
    if (reschedule) {
        UiManager::GetInstance().Dispatch([this]() { DrainIatText(); });
    }
}

void VoiceSubtitleController::QueueTtsAudioProgress(uint32_t audio_clock_ms) {
    uint32_t observed = latest_tts_audio_clock_ms_.load(std::memory_order_relaxed);
    while (observed < audio_clock_ms &&
           !latest_tts_audio_clock_ms_.compare_exchange_weak(
               observed, audio_clock_ms, std::memory_order_release,
               std::memory_order_relaxed)) {
    }
    bool expected = false;
    if (!tts_progress_dispatch_pending_.compare_exchange_strong(expected, true)) return;
    const uint32_t generation = tts_progress_generation_.load();
    UiManager::GetInstance().Dispatch([this, generation]() {
        if (generation != tts_progress_generation_.load()) return;
        const uint32_t handled_clock = latest_tts_audio_clock_ms_.load();
        OnTtsAudioProgress(handled_clock);
        tts_progress_dispatch_pending_.store(false);
        if (generation == tts_progress_generation_.load() &&
            latest_tts_audio_clock_ms_.load() > handled_clock) {
            QueueTtsAudioProgress(latest_tts_audio_clock_ms_.load());
        }
    });
}

void VoiceSubtitleController::ResetSubtitles(bool clear_text) {
    subtitle_segments_.clear();
    subtitle_index_ = 0;
    subtitle_audio_started_ = false;
    subtitle_audio_clock_ms_ = 0;
    if (clear_text && presenter_.view_ != nullptr) {
        presenter_.view_->SetSubtitleText("");
    }
}

void VoiceSubtitleController::OnTtsTextStart() {
    if (presenter_.view_ == nullptr) return;
    const bool audio_started = subtitle_audio_started_;
    const uint32_t audio_clock_ms = subtitle_audio_clock_ms_;
    iat_text_visible_ = false;
    ResetSubtitles(true);
    if (audio_started) {
        subtitle_audio_started_ = true;
        subtitle_audio_clock_ms_ = audio_clock_ms;
    }
    ESP_LOGI(TAG, "字幕 UI 流开始: audio_started=%d, clock=%lums",
             subtitle_audio_started_ ? 1 : 0,
             static_cast<unsigned long>(subtitle_audio_clock_ms_));
}

void VoiceSubtitleController::OnTtsTextUpdate(const std::string& text) {
    if (presenter_.view_ == nullptr || text.empty()) return;
    cJSON* root = cJSON_ParseWithLength(text.data(), text.size());
    if (root == nullptr) {
        ESP_LOGW(TAG, "字幕时间段 JSON 解析失败，已忽略: bytes=%u",
                 static_cast<unsigned>(text.size()));
        return;
    }
    std::function<void(const cJSON*)> add_segment = [this](const cJSON* item) {
        if (!cJSON_IsObject(item)) return;
        const auto* text_item = cJSON_GetObjectItemCaseSensitive(item, "text");
        uint32_t cloud_index = 0, start_ms = 0, end_ms = 0;
        const bool valid = cJSON_IsString(text_item) && text_item->valuestring != nullptr &&
            text_item->valuestring[0] != '\0' &&
            ReadJsonUint32(item, "index", &cloud_index) &&
            ReadJsonUint32(item, "start_ms", &start_ms) &&
            ReadJsonUint32(item, "end_ms", &end_ms) && end_ms > start_ms;
        if (!valid) {
            ESP_LOGW(TAG, "字幕时间段字段无效，已忽略");
            return;
        }
        if ((subtitle_segments_.empty() && cloud_index != 0) ||
            (!subtitle_segments_.empty() &&
             (cloud_index != subtitle_segments_.back().cloud_index + 1 ||
              start_ms < subtitle_segments_.back().end_ms))) {
            ESP_LOGW(TAG, "字幕时间段乱序或重复，已忽略: index=%lu, start=%lums",
                     static_cast<unsigned long>(cloud_index),
                     static_cast<unsigned long>(start_ms));
            return;
        }
        subtitle_segments_.push_back({cloud_index, text_item->valuestring, start_ms, end_ms});
    };
    if (cJSON_IsArray(root)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) { add_segment(item); }
    } else {
        const cJSON* item = root;
        const auto* nested = cJSON_IsObject(root)
            ? cJSON_GetObjectItemCaseSensitive(root, "data") : nullptr;
        if (nested != nullptr && cJSON_IsObject(nested)) item = nested;
        add_segment(item);
    }
    ScheduleSubtitle();
    cJSON_Delete(root);
}

void VoiceSubtitleController::OnTtsTextInterrupt() {
    if (presenter_.view_ != nullptr) ResetSubtitles(true);
}

void VoiceSubtitleController::OnTtsPlaying() {
    if (presenter_.view_ == nullptr) return;
    presenter_.SetStatus(Lang::Strings::SPEAKING);
    presenter_.SetEmotion("说话");
    subtitle_audio_started_ = true;
    subtitle_audio_clock_ms_ = 0;
    ESP_LOGI(TAG, "字幕 UI 音频时钟启动");
    ScheduleSubtitle();
}

void VoiceSubtitleController::OnTtsAudioProgress(uint32_t audio_clock_ms) {
    if (presenter_.view_ == nullptr || !subtitle_audio_started_) return;
    subtitle_audio_clock_ms_ = std::max(subtitle_audio_clock_ms_, audio_clock_ms);
    ScheduleSubtitle();
}

void VoiceSubtitleController::OnTtsStopped() {
    if (presenter_.view_ == nullptr) return;
    ResetSubtitles(true);
    const auto state = DeviceStatus::GetState();
    if (state == kDeviceStateIdle) {
        presenter_.SetStatus(Lang::Strings::STANDBY);
        presenter_.SetEmotion("待机");
    } else if (state == kDeviceStateListening) {
        presenter_.SetStatus(Lang::Strings::LISTENING);
        presenter_.SetEmotion("聆听");
    }
}

void VoiceSubtitleController::ScheduleSubtitle() {
    if (iat_text_visible_ || !subtitle_audio_started_ ||
        subtitle_index_ >= subtitle_segments_.size()) return;
    const uint64_t elapsed_ms = subtitle_audio_clock_ms_;
    while (subtitle_index_ < subtitle_segments_.size() &&
           subtitle_segments_[subtitle_index_].end_ms <= elapsed_ms) {
        ++subtitle_index_;
    }
    if (subtitle_index_ >= subtitle_segments_.size()) return;
    const auto& segment = subtitle_segments_[subtitle_index_];
    if (elapsed_ms < segment.start_ms) return;
    if (presenter_.view_ != nullptr) {
        presenter_.view_->SetSubtitleText(segment.text);
        ESP_LOGI(TAG, "字幕 UI 显示: cloud_index=%lu, queue_index=%u, bytes=%u, interval=%lu-%lums, clock=%lums",
                 static_cast<unsigned long>(segment.cloud_index),
                 static_cast<unsigned>(subtitle_index_),
                 static_cast<unsigned>(segment.text.size()),
                 static_cast<unsigned long>(segment.start_ms),
                 static_cast<unsigned long>(segment.end_ms),
                 static_cast<unsigned long>(elapsed_ms));
    }
    ++subtitle_index_;
}
