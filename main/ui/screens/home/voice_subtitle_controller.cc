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
#include <limits>
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

bool DecodeUtf8(const std::string& text, size_t offset, uint32_t* codepoint,
                size_t* next_offset) {
    if (offset >= text.size() || codepoint == nullptr || next_offset == nullptr) {
        return false;
    }
    const auto lead = static_cast<uint8_t>(text[offset]);
    size_t length = 1;
    uint32_t value = lead;
    if ((lead & 0xe0) == 0xc0) {
        length = 2;
        value = lead & 0x1f;
    } else if ((lead & 0xf0) == 0xe0) {
        length = 3;
        value = lead & 0x0f;
    } else if ((lead & 0xf8) == 0xf0) {
        length = 4;
        value = lead & 0x07;
    } else if ((lead & 0x80) != 0) {
        *codepoint = 0xfffd;
        *next_offset = offset + 1;
        return true;
    }
    if (offset + length > text.size()) {
        *codepoint = 0xfffd;
        *next_offset = text.size();
        return true;
    }
    for (size_t i = 1; i < length; ++i) {
        const auto continuation = static_cast<uint8_t>(text[offset + i]);
        if ((continuation & 0xc0) != 0x80) {
            *codepoint = 0xfffd;
            *next_offset = offset + 1;
            return true;
        }
        value = (value << 6) | (continuation & 0x3f);
    }
    *codepoint = value;
    *next_offset = offset + length;
    return true;
}

uint32_t PlainSubtitleDelayMs(uint32_t codepoint) {
    switch (codepoint) {
        case '\n':
        case '\r':
            return 240;
        case ',':
        case ';':
        case ':':
        case 0x3001:  // 、
        case 0xff0c:  // ，
        case 0xff1b:  // ；
        case 0xff1a:  // ：
            return 210;
        case '.':
        case '!':
        case '?':
        case 0x3002:  // 。
        case 0xff01:  // ！
        case 0xff1f:  // ？
        case 0x2026:  // …
            return 360;
        case ' ':
        case '\t':
            return 30;
        default:
            return codepoint < 0x80 ? 55 : 120;
    }
}

bool IsPlainSubtitleBoundary(uint32_t codepoint) {
    switch (codepoint) {
        case ';':
        case '.':
        case '!':
        case '?':
        case '\n':
        case '\r':
        case 0x3002:  // 。
        case 0xff1b:  // ；
        case 0xff01:  // ！
        case 0xff1f:  // ？
        case 0x2026:  // …
            return true;
        default:
            return false;
    }
}

}  // namespace

VoiceSubtitleController::VoiceSubtitleController(HomePresenter& presenter)
    : presenter_(presenter) {
}

void VoiceSubtitleController::Start() {
    if (!event_subscriptions_.empty()) {
        return;
    }
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::VoiceSessionStarted, [this](const AppEvent::Event& event) {
            const auto scope = event.Scope();
            UiManager::GetInstance().Dispatch([this, scope]() {
                if (!scope.IsCurrent()) return;
                tts_progress_generation_.fetch_add(1);
                latest_tts_audio_clock_ms_.store(0);
                tts_progress_dispatch_pending_.store(false);
                ResetSubtitles(true);
                iat_text_visible_ = false;
            });
        }));
    auto subscribe_text = [this](AppEvent::Id id) {
        event_subscriptions_.push_back(AppEvent::Subscribe(
            id, [this](const AppEvent::Event& event) {
                QueueIatText(std::string(event.Text()), event.Scope());
            }));
    };
    subscribe_text(AppEvent::Id::XiaolingIatStart);
    subscribe_text(AppEvent::Id::XiaolingIatUpdate);
    subscribe_text(AppEvent::Id::XiaolingIatEnd);
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsTextStart, [this](const AppEvent::Event& event) {
            const auto scope = event.Scope();
            UiManager::GetInstance().Dispatch([this, scope]() { if (scope.IsCurrent()) OnTtsTextStart(); });
        }));
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsTextUpdate, [this](const AppEvent::Event& event) {
            UiManager::GetInstance().Dispatch(
                [this, text = std::string(event.Text()), scope = event.Scope()]() {
                    if (scope.IsCurrent()) OnTtsTextUpdate(text);
                });
        }));
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsTextEnd, [this](const AppEvent::Event& event) {
            const auto scope = event.Scope();
            UiManager::GetInstance().Dispatch([this, scope]() { if (scope.IsCurrent()) OnTtsTextEnd(); });
        }));
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsTextInterrupt, [this](const AppEvent::Event& event) {
            const auto scope = event.Scope();
            UiManager::GetInstance().Dispatch([this, scope]() { if (scope.IsCurrent()) OnTtsTextInterrupt(); });
        }));
    event_subscriptions_.push_back(AppEvent::Subscribe(
        AppEvent::Id::XiaolingTtsStarted, [this](const AppEvent::Event& event) {
            const auto scope = event.Scope();
            const uint32_t generation = tts_progress_generation_.fetch_add(1) + 1;
            latest_tts_audio_clock_ms_.store(0);
            tts_progress_dispatch_pending_.store(false);
            UiManager::GetInstance().Dispatch([this, generation, scope]() {
                if (scope.IsCurrent() && generation == tts_progress_generation_.load()) {
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
        AppEvent::Id::XiaolingTtsStopped, [this](const AppEvent::Event& event) {
            const auto scope = event.Scope();
            const uint32_t generation = tts_progress_generation_.fetch_add(1) + 1;
            latest_tts_audio_clock_ms_.store(0);
            tts_progress_dispatch_pending_.store(false);
            UiManager::GetInstance().Dispatch([this, generation, scope]() {
                if (scope.IsCurrent() && generation == tts_progress_generation_.load()) {
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

void VoiceSubtitleController::QueueIatText(std::string text, AppEvent::EpochSnapshot scope) {
    if (text.empty()) return;
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(iat_text_mutex_);
        pending_iat_text_ = std::move(text);
        pending_iat_scope_ = scope;
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
    AppEvent::EpochSnapshot scope;
    {
        std::lock_guard<std::mutex> lock(iat_text_mutex_);
        text = pending_iat_text_;
        scope = pending_iat_scope_;
        revision = last_voice_text_revision_;
        received_us = pending_iat_received_us_;
    }
    if (!text.empty() && scope.IsCurrent()) {
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
    plain_subtitle_text_.clear();
    plain_subtitle_cues_.clear();
    plain_subtitle_cue_index_ = 0;
    plain_subtitle_visible_start_byte_ = 0;
    plain_subtitle_visible_bytes_ = 0;
    plain_subtitle_scheduled_bytes_ = 0;
    plain_subtitle_next_due_ms_ = 0;
    plain_subtitle_mode_ = false;
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
        AppendPlainSubtitle(text);
        return;
    }
    std::string plain_text;
    bool timeline_added = false;
    if (cJSON_IsString(root) && root->valuestring != nullptr) {
        plain_text = root->valuestring;
    }
    std::function<void(const cJSON*)> add_segment = [this, &timeline_added](const cJSON* item) {
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
        timeline_added = true;
    };
    if (cJSON_IsArray(root)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) { add_segment(item); }
    } else {
        const cJSON* item = root;
        const auto* nested = cJSON_IsObject(root)
            ? cJSON_GetObjectItemCaseSensitive(root, "data") : nullptr;
        if (cJSON_IsString(nested) && nested->valuestring != nullptr) {
            plain_text = nested->valuestring;
        } else if (nested != nullptr && cJSON_IsObject(nested)) {
            item = nested;
        }
        const auto* plain_item = cJSON_IsObject(item)
            ? cJSON_GetObjectItemCaseSensitive(item, "text") : nullptr;
        if (cJSON_IsString(plain_item) && plain_item->valuestring != nullptr &&
            cJSON_GetObjectItemCaseSensitive(item, "start_ms") == nullptr) {
            plain_text = plain_item->valuestring;
        }
        if (plain_text.empty()) {
            add_segment(item);
        }
    }
    if (!plain_text.empty()) {
        AppendPlainSubtitle(plain_text);
    } else if (timeline_added) {
        plain_subtitle_mode_ = false;
        ScheduleSubtitle();
    }
    cJSON_Delete(root);
}

void VoiceSubtitleController::OnTtsTextEnd() {
    if (plain_subtitle_mode_) {
        BuildPlainSubtitleCues(true);
        SchedulePlainSubtitle();
    }
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
        presenter_.SetStatus(Lang::Strings::STANDBY, true);
        presenter_.SetEmotion("待机");
    } else if (state == kDeviceStateListening) {
        presenter_.SetStatus(Lang::Strings::LISTENING);
        presenter_.SetEmotion("聆听");
    }
}

void VoiceSubtitleController::ScheduleSubtitle() {
    if (plain_subtitle_mode_) {
        SchedulePlainSubtitle();
        return;
    }
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
        presenter_.view_->SetSubtitleText(segment.text, 3);
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

void VoiceSubtitleController::AppendPlainSubtitle(const std::string& text) {
    if (text.empty()) {
        return;
    }
    plain_subtitle_mode_ = true;

    // 同时兼容云端返回的增量分片和累计快照，并避免重复拼接已接收文本。
    std::string delta;
    if (text.size() >= plain_subtitle_text_.size() &&
        text.compare(0, plain_subtitle_text_.size(), plain_subtitle_text_) == 0) {
        delta = text.substr(plain_subtitle_text_.size());
    } else if (plain_subtitle_text_.size() >= text.size() &&
               plain_subtitle_text_.compare(
                   plain_subtitle_text_.size() - text.size(), text.size(), text) == 0) {
        return;
    } else {
        delta = text;
    }
    if (delta.empty()) {
        return;
    }

    plain_subtitle_text_ += delta;
    BuildPlainSubtitleCues(false);
    ESP_LOGI(TAG,
             "普通字幕分句: append=%u, total=%u, scheduled=%u, phrases=%u, clock=%lums, end=%lums",
             static_cast<unsigned>(delta.size()),
             static_cast<unsigned>(plain_subtitle_text_.size()),
             static_cast<unsigned>(plain_subtitle_scheduled_bytes_),
             static_cast<unsigned>(plain_subtitle_cues_.size()),
             static_cast<unsigned long>(subtitle_audio_clock_ms_),
             static_cast<unsigned long>(plain_subtitle_next_due_ms_));
    SchedulePlainSubtitle();
}

void VoiceSubtitleController::BuildPlainSubtitleCues(bool flush_pending) {
    // 优先按完整句子切分。逗号、顿号和冒号只影响时长，不再生成过碎的
    // 字幕片段；若云端文本迟迟没有句末标点，则在约两行处提前切分，
    // 避免下一条字幕无限等待。
    constexpr size_t kMaxPhraseCodepoints = 26;
    size_t offset = plain_subtitle_scheduled_bytes_;
    size_t phrase_start = plain_subtitle_scheduled_bytes_;
    size_t phrase_codepoints = 0;
    uint32_t phrase_duration_ms = 0;
    while (offset < plain_subtitle_text_.size()) {
        uint32_t codepoint = 0;
        size_t next_offset = offset;
        if (!DecodeUtf8(plain_subtitle_text_, offset, &codepoint, &next_offset) ||
            next_offset <= offset) {
            break;
        }
        ++phrase_codepoints;
        phrase_duration_ms += PlainSubtitleDelayMs(codepoint);
        offset = next_offset;

        const bool boundary = IsPlainSubtitleBoundary(codepoint) ||
                              phrase_codepoints >= kMaxPhraseCodepoints;
        if (!boundary) {
            continue;
        }
        plain_subtitle_next_due_ms_ =
            std::max(plain_subtitle_next_due_ms_, subtitle_audio_clock_ms_);
        plain_subtitle_cues_.push_back(
            {phrase_start, offset, plain_subtitle_next_due_ms_});
        const uint64_t next_due =
            static_cast<uint64_t>(plain_subtitle_next_due_ms_) +
            phrase_duration_ms;
        plain_subtitle_next_due_ms_ = static_cast<uint32_t>(
            std::min<uint64_t>(next_due, std::numeric_limits<uint32_t>::max()));
        plain_subtitle_scheduled_bytes_ = offset;
        phrase_start = offset;
        phrase_codepoints = 0;
        phrase_duration_ms = 0;
    }

    if (flush_pending && plain_subtitle_scheduled_bytes_ < plain_subtitle_text_.size()) {
        // 未排期的尾段可能跨越多个网络分片，且没有标点、长度又未达到
        // 提前切分阈值，因此在文本流结束时重新计算并补入队列。
        uint32_t tail_duration_ms = 0;
        offset = plain_subtitle_scheduled_bytes_;
        while (offset < plain_subtitle_text_.size()) {
            uint32_t codepoint = 0;
            size_t next_offset = offset;
            if (!DecodeUtf8(plain_subtitle_text_, offset, &codepoint, &next_offset) ||
                next_offset <= offset) {
                break;
            }
            tail_duration_ms += PlainSubtitleDelayMs(codepoint);
            offset = next_offset;
        }
        plain_subtitle_next_due_ms_ =
            std::max(plain_subtitle_next_due_ms_, subtitle_audio_clock_ms_);
        plain_subtitle_cues_.push_back(
            {plain_subtitle_scheduled_bytes_, plain_subtitle_text_.size(),
             plain_subtitle_next_due_ms_});
        const uint64_t next_due =
            static_cast<uint64_t>(plain_subtitle_next_due_ms_) + tail_duration_ms;
        plain_subtitle_next_due_ms_ = static_cast<uint32_t>(
            std::min<uint64_t>(next_due, std::numeric_limits<uint32_t>::max()));
        plain_subtitle_scheduled_bytes_ = plain_subtitle_text_.size();
    }
}

void VoiceSubtitleController::SchedulePlainSubtitle() {
    if (iat_text_visible_ || !subtitle_audio_started_ ||
        !plain_subtitle_mode_ || presenter_.view_ == nullptr) {
        return;
    }
    const size_t previous_visible = plain_subtitle_visible_bytes_;
    while (plain_subtitle_cue_index_ < plain_subtitle_cues_.size() &&
        plain_subtitle_cues_[plain_subtitle_cue_index_].show_at_ms <=
               subtitle_audio_clock_ms_) {
        plain_subtitle_visible_start_byte_ =
            plain_subtitle_cues_[plain_subtitle_cue_index_].start_byte;
        plain_subtitle_visible_bytes_ =
            plain_subtitle_cues_[plain_subtitle_cue_index_].end_byte;
        ++plain_subtitle_cue_index_;
    }
    if (plain_subtitle_visible_bytes_ == 0 ||
        plain_subtitle_visible_bytes_ == previous_visible) {
        return;
    }
    presenter_.view_->SetSubtitleText(
        plain_subtitle_text_.substr(
            plain_subtitle_visible_start_byte_,
            plain_subtitle_visible_bytes_ - plain_subtitle_visible_start_byte_),
        2);
}
