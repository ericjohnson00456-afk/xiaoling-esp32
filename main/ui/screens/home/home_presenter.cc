#include "home_presenter.h"

#include "display/display.h"
#include "board.h"
#include "home_view.h"
#include "ui/i18n/lang_config.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"
#include "ui/utils/image/lvgl_image.h"
#include "xiaoling/xiaoling_events.h"

#include <utility>
#include <cstring>
#include <algorithm>
#include <cstdlib>

#include <esp_log.h>

#define TAG "HomePresenter"

#ifdef CONFIG_LSPLATFORM
namespace {

class SharedLvglImage final : public LvglImage {
public:
    explicit SharedLvglImage(std::shared_ptr<LvglImage> image) : image_(std::move(image)) {
    }

    const lv_img_dsc_t* image_dsc() const override {
        return image_ != nullptr ? image_->image_dsc() : nullptr;
    }

private:
    std::shared_ptr<LvglImage> image_;
};

}  // namespace
#endif

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

HomePresenter& HomePresenter::GetInstance() {
    static HomePresenter instance;
    return instance;
}

const UiNavScreen& HomePresenter::Screen() {
    static const UiNavScreen screen = {
        .unique_id = UI_PAGE_HOME,
        .open = &HomePresenter::Open,
        .show = &HomePresenter::Show,
        .pause = &HomePresenter::Pause,
        .resume = &HomePresenter::Resume,
        .close = &HomePresenter::Close,
    };
    return screen;
}

void HomePresenter::SetBannerIdleProvider(std::function<bool()> provider) {
    std::lock_guard<std::mutex> lock(banner_mutex_);
    banner_idle_provider_ = std::move(provider);
}

void HomePresenter::SetWakeWordProvider(std::function<std::string()> provider) {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    wake_word_provider_ = std::move(provider);
}

int HomePresenter::Open(const UiNavScreen* screen, void** data) {
    (void)screen;
    auto& presenter = GetInstance();
    presenter.view_ = std::make_unique<HomeView>(UiManager::GetInstance().display());
    presenter.SubscribeVoiceEvents();
    *data = &presenter;
    return 0;
}

int HomePresenter::Show(const UiNavScreen* screen, void* data) {
    (void)screen;
    auto* presenter = static_cast<HomePresenter*>(data);
    if (presenter == nullptr || presenter->view_ == nullptr) {
        return -1;
    }
    presenter->view_->Show();
    presenter->SetEmotion(presenter->last_emotion_);
    presenter->SetChatMessage(presenter->last_chat_role_, presenter->last_chat_text_);
    presenter->SetText(presenter->last_text_);
    presenter->SetWifiIcon(presenter->wifi_icon_);
    presenter->UpdateBattery();
    presenter->SetStatus(presenter->status_text_);
    presenter->SetTime(presenter->time_text_);
    presenter->SetDuplexVisible(presenter->duplex_visible_);
#ifdef CONFIG_LSPLATFORM
    if (presenter->image_ != nullptr) {
        presenter->view_->ShowImage(std::make_unique<SharedLvglImage>(presenter->image_));
    }
#endif
    return 0;
}

int HomePresenter::Pause(const UiNavScreen* screen, void* data) {
    (void)screen;
    auto* presenter = static_cast<HomePresenter*>(data);
    if (presenter != nullptr && presenter->view_ != nullptr) {
        presenter->view_->Hide();
    }
    return 0;
}

int HomePresenter::Resume(const UiNavScreen* screen, void* data) {
    return Show(screen, data);
}

int HomePresenter::Close(const UiNavScreen* screen, void* data) {
    (void)screen;
    auto* presenter = static_cast<HomePresenter*>(data);
    if (presenter == nullptr) {
        return 0;
    }
    presenter->UnsubscribeVoiceEvents();
    presenter->StopBannerTimer();
    presenter->ResetSubtitles(true);
    presenter->view_.reset();
    presenter->last_emotion_.clear();
    presenter->last_chat_role_.clear();
    presenter->last_chat_text_.clear();
    {
        std::lock_guard<std::mutex> lock(presenter->iat_text_mutex_);
        presenter->pending_iat_text_.clear();
        presenter->pending_iat_received_us_ = 0;
        presenter->iat_dispatch_pending_ = false;
        ++presenter->last_voice_text_revision_;
    }
    presenter->iat_text_visible_ = false;
    presenter->tts_progress_generation_.fetch_add(1);
    presenter->latest_tts_audio_clock_ms_.store(0);
    presenter->tts_progress_dispatch_pending_.store(false);
    return 0;
}

void HomePresenter::SetEmotion(std::string emotion) {
    last_emotion_ = std::move(emotion);
    if (view_ != nullptr && !last_emotion_.empty()) {
        view_->SetEmotion(last_emotion_);
    }
}

void HomePresenter::ShowEmotionOnce(std::string emotion) {
    if (view_ != nullptr) {
        view_->ShowEmotionOnce(emotion);
    }
}

void HomePresenter::SetPowerSaveMode(bool enabled) {
    SetEmotion(enabled ? "sleepy" : "neutral");
    SetChatMessage("system", "");
}

void HomePresenter::SubscribeVoiceEvents() {
    auto& events = XiaolingEventBus::GetInstance();
    auto subscribe_text = [this, &events](XiaolingEventType type) {
        voice_event_subscriptions_.push_back(events.Subscribe(type, [this](const XiaolingEvent& event) {
            QueueIatText(event.text);
        }));
    };
    subscribe_text(XIAOLING_IAT_START);
    subscribe_text(XIAOLING_IAT_UPDATE);
    subscribe_text(XIAOLING_IAT_END);
    voice_event_subscriptions_.push_back(events.Subscribe(
        XIAOLING_TTS_TXT_START, [this](const XiaolingEvent&) {
            UiManager::GetInstance().Dispatch([this]() { OnTtsTextStart(); });
        }));
    voice_event_subscriptions_.push_back(events.Subscribe(
        XIAOLING_TTS_TXT_UPDATE, [this](const XiaolingEvent& event) {
            UiManager::GetInstance().Dispatch(
                [this, text = event.text]() { OnTtsTextUpdate(text); });
        }));
    voice_event_subscriptions_.push_back(events.Subscribe(
        XIAOLING_TTS_TXT_INTERRUPT, [this](const XiaolingEvent&) {
            UiManager::GetInstance().DispatchUrgent(
                [this]() { OnTtsTextInterrupt(); });
        }));
    voice_event_subscriptions_.push_back(events.Subscribe(
        XIAOLING_TTS_OUTPUT_STARTED, [this](const XiaolingEvent&) {
            const uint32_t generation = tts_progress_generation_.fetch_add(1) + 1;
            latest_tts_audio_clock_ms_.store(0);
            tts_progress_dispatch_pending_.store(false);
            UiManager::GetInstance().DispatchUrgent([this, generation]() {
                if (generation == tts_progress_generation_.load()) {
                    OnTtsPlaying();
                }
            });
        }));
    voice_event_subscriptions_.push_back(events.Subscribe(
        XIAOLING_TTS_OUTPUT_PROGRESS, [this](const XiaolingEvent& event) {
            uint32_t audio_clock_ms = 0;
            if (!event.text.empty()) {
                char* end = nullptr;
                const unsigned long value = std::strtoul(event.text.c_str(), &end, 10);
                if (end != event.text.c_str() && *end == '\0' && value <= UINT32_MAX) {
                    audio_clock_ms = static_cast<uint32_t>(value);
                }
            }
            QueueTtsAudioProgress(audio_clock_ms);
        }));
    voice_event_subscriptions_.push_back(events.Subscribe(
        XIAOLING_TTS_STOPPED, [this](const XiaolingEvent&) {
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

void HomePresenter::QueueIatText(std::string text) {
    if (text.empty()) {
        return;
    }
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
        UiManager::GetInstance().DispatchUrgent([this]() { DrainIatText(); });
    }
}

void HomePresenter::DrainIatText() {
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
            // In full duplex DeviceStatus intentionally remains listening
            // during TTS, so its state callback cannot restore this UI. Let
            // the first real IAT take ownership immediately, before GMF stop
            // waits for the HTTP/decode pipeline to drain.
            ResetSubtitles(true);
            SetStatus(Lang::Strings::LISTENING);
            SetEmotion("聆听");
            ESP_LOGI(TAG, "非空 IAT 已接管 TTS UI，字幕时钟停止");
        }
        iat_text_visible_ = true;
        SetText(std::move(text));
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
        UiManager::GetInstance().DispatchUrgent([this]() { DrainIatText(); });
    }
}

void HomePresenter::QueueTtsAudioProgress(uint32_t audio_clock_ms) {
    uint32_t observed = latest_tts_audio_clock_ms_.load(std::memory_order_relaxed);
    while (observed < audio_clock_ms &&
           !latest_tts_audio_clock_ms_.compare_exchange_weak(
               observed, audio_clock_ms, std::memory_order_release,
               std::memory_order_relaxed)) {
    }

    bool expected = false;
    if (!tts_progress_dispatch_pending_.compare_exchange_strong(expected, true)) {
        return;
    }
    const uint32_t generation = tts_progress_generation_.load();
    UiManager::GetInstance().DispatchUrgent([this, generation]() {
        if (generation != tts_progress_generation_.load()) {
            return;
        }
        const uint32_t handled_clock = latest_tts_audio_clock_ms_.load();
        OnTtsAudioProgress(handled_clock);
        tts_progress_dispatch_pending_.store(false);
        if (generation == tts_progress_generation_.load() &&
            latest_tts_audio_clock_ms_.load() > handled_clock) {
            QueueTtsAudioProgress(latest_tts_audio_clock_ms_.load());
        }
    });
}

void HomePresenter::UnsubscribeVoiceEvents() {
    auto& events = XiaolingEventBus::GetInstance();
    for (const auto id : voice_event_subscriptions_) {
        events.Unsubscribe(id);
    }
    voice_event_subscriptions_.clear();
}

void HomePresenter::ResetSubtitles(bool clear_text) {
    subtitle_segments_.clear();
    subtitle_index_ = 0;
    subtitle_audio_started_ = false;
    subtitle_audio_clock_ms_ = 0;
    if (clear_text && view_ != nullptr) {
        view_->SetSubtitleText("");
    }
}

void HomePresenter::OnTtsTextStart() {
    if (view_ == nullptr) {
        return;
    }
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

void HomePresenter::OnTtsTextUpdate(const std::string& text) {
    if (view_ == nullptr || text.empty()) {
        return;
    }
    cJSON* root = cJSON_ParseWithLength(text.data(), text.size());
    if (root == nullptr) {
        ESP_LOGW(TAG, "字幕时间段 JSON 解析失败，已忽略: bytes=%u",
                 static_cast<unsigned>(text.size()));
        return;
    }
    std::function<void(const cJSON*)> add_segment = [this](const cJSON* item) {
        if (!cJSON_IsObject(item)) {
            return;
        }
        const auto* text_item = cJSON_GetObjectItemCaseSensitive(item, "text");
        uint32_t cloud_index = 0;
        uint32_t start_ms = 0;
        uint32_t end_ms = 0;
        const bool valid = cJSON_IsString(text_item) && text_item->valuestring != nullptr &&
            text_item->valuestring[0] != '\0' &&
            ReadJsonUint32(item, "index", &cloud_index) &&
            ReadJsonUint32(item, "start_ms", &start_ms) &&
            ReadJsonUint32(item, "end_ms", &end_ms) &&
            end_ms > start_ms;
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
        subtitle_segments_.push_back(
            {cloud_index, text_item->valuestring, start_ms, end_ms});
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

void HomePresenter::OnTtsTextInterrupt() {
    if (view_ == nullptr) {
        return;
    }
    ResetSubtitles(true);
}

void HomePresenter::OnTtsPlaying() {
    if (view_ == nullptr) {
        return;
    }
    // This callback is already marshalled to the UI/application task from the
    // first PCM output event. Update the visible state here instead of waiting
    // for the subsequent DeviceStatus notification cycle. The device state is
    // still transitioned by Application and remains the single source of truth.
    SetStatus(Lang::Strings::SPEAKING);
    SetEmotion("说话");
    subtitle_audio_started_ = true;
    subtitle_audio_clock_ms_ = 0;
    ESP_LOGI(TAG, "字幕 UI 音频时钟启动");
    ScheduleSubtitle();
}

void HomePresenter::OnTtsAudioProgress(uint32_t audio_clock_ms) {
    if (view_ == nullptr || !subtitle_audio_started_) {
        return;
    }
    subtitle_audio_clock_ms_ = std::max(subtitle_audio_clock_ms_, audio_clock_ms);
    ScheduleSubtitle();
}

void HomePresenter::OnTtsStopped() {
    if (view_ == nullptr) {
        return;
    }
    ResetSubtitles(true);
}

void HomePresenter::ScheduleSubtitle() {
    if (iat_text_visible_ || !subtitle_audio_started_ ||
        subtitle_index_ >= subtitle_segments_.size()) {
        return;
    }

    const uint64_t elapsed_ms = subtitle_audio_clock_ms_;
    while (subtitle_index_ < subtitle_segments_.size() &&
           subtitle_segments_[subtitle_index_].end_ms <= elapsed_ms) {
        ++subtitle_index_;
    }
    if (subtitle_index_ >= subtitle_segments_.size()) {
        return;
    }

    const auto& segment = subtitle_segments_[subtitle_index_];
    if (elapsed_ms < segment.start_ms) {
        return;
    }
    if (view_ != nullptr) {
        view_->SetSubtitleText(segment.text);
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

void HomePresenter::SetText(std::string text) {
    last_text_ = std::move(text);
    if (view_ != nullptr && !last_text_.empty()) {
        view_->SetText(last_text_);
    }
}

void HomePresenter::SetChatMessage(std::string role, std::string text) {
    last_chat_role_ = role;
    last_chat_text_ = text;
    if (view_ != nullptr) {
        view_->SetChatMessage(std::move(role), std::move(text));
    }
}

void HomePresenter::ShowNotification(std::string text, int duration_ms) {
    if (view_ != nullptr && !text.empty()) {
        view_->ShowNotification(text, duration_ms);
    }
}

void HomePresenter::SetWifiIcon(std::string icon) {
    wifi_icon_ = std::move(icon);
    if (view_ != nullptr) {
        view_->SetWifiIcon(wifi_icon_);
    }
}

void HomePresenter::UpdateBattery() {
    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        return;
    }

    battery_level_ = level;
    battery_charging_ = charging;
    if (view_ != nullptr) {
        view_->SetBattery(battery_level_, battery_charging_);
    }
}

void HomePresenter::SetStatus(std::string status) {
    status_text_ = std::move(status);
    if (view_ != nullptr) {
        view_->SetStatus(status_text_);
    }
}

void HomePresenter::SetTime(std::string time) {
    time_text_ = std::move(time);
    if (view_ != nullptr && !time_text_.empty()) {
        view_->SetTime(time_text_);
    }
}

void HomePresenter::SetDuplexMode(DuplexMode mode) {
    duplex_mode_ = mode;
    if (duplex_visible_ && view_ != nullptr) {
        view_->SetDuplexMode(mode);
    }
}

void HomePresenter::SetDuplexVisible(bool visible) {
    duplex_visible_ = visible;
    if (view_ != nullptr) {
        view_->SetDuplexMode(visible ? duplex_mode_ : DuplexMode::kSimplex);
    }
}

#ifdef CONFIG_LSPLATFORM
void HomePresenter::SetImage(std::unique_ptr<LvglImage> image) {
    if (image == nullptr) {
        return;
    }
    auto shared = std::shared_ptr<LvglImage>(std::move(image));
    UiManager::GetInstance().Dispatch([shared = std::move(shared)]() mutable {
        auto& home = HomePresenter::GetInstance();
        home.image_ = std::move(shared);
        if (home.view_ != nullptr && home.image_ != nullptr &&
            UiManager::GetInstance().TopScreenId() == UI_PAGE_HOME) {
            home.view_->ShowImage(std::make_unique<SharedLvglImage>(home.image_));
        }
    });
}

void HomePresenter::ClearImage() {
    UiManager::GetInstance().Dispatch([]() {
        auto& home = HomePresenter::GetInstance();
        home.image_.reset();
        if (home.view_ != nullptr) {
            home.view_->HideImage();
        }
    });
}
#endif

void HomePresenter::HandleSettingMessage(const cJSON* data) {
    const auto* theme = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "theme") : nullptr;
    const auto* frontend = cJSON_IsObject(theme) ? cJSON_GetObjectItemCaseSensitive(theme, "frontend") : nullptr;
    const auto* banner = cJSON_IsObject(frontend) ? cJSON_GetObjectItemCaseSensitive(frontend, "banner") : nullptr;
    if (!cJSON_IsObject(banner)) {
        return;
    }

    uint32_t interval_ms = 3000;
    const auto* interval = cJSON_GetObjectItemCaseSensitive(banner, "interval_ms");
    if (cJSON_IsNumber(interval) && interval->valueint >= 1000) {
        interval_ms = static_cast<uint32_t>(interval->valueint);
    }

    std::vector<std::string> texts;
    const auto* resources = cJSON_GetObjectItemCaseSensitive(banner, "resources");
    if (cJSON_IsArray(resources)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, resources) {
            const auto* text = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "text") : nullptr;
            if (cJSON_IsString(text) && text->valuestring != nullptr && text->valuestring[0] != '\0') {
                texts.push_back(SubstituteWakeWord(text->valuestring));
            }
        }
    }
    UiManager::GetInstance().Dispatch([texts = std::move(texts), interval_ms]() mutable {
        HomePresenter::GetInstance().SetBanners(std::move(texts), interval_ms);
    });
}

std::string HomePresenter::SubstituteWakeWord(const std::string& text) const {
    static constexpr const char* kPlaceholder = "#唤醒词#";
    if (text.find(kPlaceholder) == std::string::npos) {
        return text;
    }
    std::function<std::string()> provider;
    {
        std::lock_guard<std::mutex> lock(wake_word_mutex_);
        provider = wake_word_provider_;
    }
    if (!provider) {
        return text;
    }
    const std::string wake_word = provider();
    if (wake_word.empty()) {
        return text;
    }
    std::string result = text;
    size_t pos = 0;
    while ((pos = result.find(kPlaceholder, pos)) != std::string::npos) {
        result.replace(pos, std::strlen(kPlaceholder), wake_word);
        pos += wake_word.size();
    }
    return result;
}

void HomePresenter::SetBanners(std::vector<std::string> texts, uint32_t interval_ms) {
    StopBannerTimer();
    bool has_banners = false;
    std::string first_text;
    {
        std::lock_guard<std::mutex> lock(banner_mutex_);
        banner_texts_ = std::move(texts);
        banner_index_ = 0;
        has_banners = !banner_texts_.empty();
        if (has_banners && (!banner_idle_provider_ || banner_idle_provider_())) {
            first_text = banner_texts_[banner_index_];
            banner_index_ = (banner_index_ + 1) % banner_texts_.size();
        }
    }
    if (!has_banners) {
        return;
    }

    // Show the first banner immediately. Waiting for the first periodic tick
    // makes startup appear blocked by the role-config image download.
    if (!first_text.empty() && view_ != nullptr) {
        view_->SetText(first_text);
        ESP_LOGI(TAG, "首条 Banner 已显示");
    }

    esp_timer_create_args_t args = {
        .callback = &HomePresenter::BannerTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "home_banner",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &banner_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "创建 Home Banner 定时器失败");
        banner_timer_ = nullptr;
        return;
    }
    esp_timer_start_periodic(banner_timer_, static_cast<uint64_t>(interval_ms) * 1000);
}

void HomePresenter::StopBannerTimer() {
    if (banner_timer_ != nullptr) {
        esp_timer_stop(banner_timer_);
        esp_timer_delete(banner_timer_);
        banner_timer_ = nullptr;
    }
}

void HomePresenter::BannerTimerCallback(void* arg) {
    static_cast<HomePresenter*>(arg)->OnBannerTick();
}

void HomePresenter::OnBannerTick() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(banner_mutex_);
        if (banner_texts_.empty() ||
            (banner_idle_provider_ && !banner_idle_provider_())) {
            return;
        }
        text = banner_texts_[banner_index_];
        banner_index_ = (banner_index_ + 1) % banner_texts_.size();
    }
    UiManager::GetInstance().Dispatch([text]() {
        auto& home = HomePresenter::GetInstance();
        if (home.view_ != nullptr) {
            home.view_->SetText(text);
        }
    });
}
