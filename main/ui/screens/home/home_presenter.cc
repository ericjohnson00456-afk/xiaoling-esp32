#include "home_presenter.h"

#include "display/display.h"
#include "home_view.h"
#include "ui/core/ui_manager.h"
#include "ui/core/ui_page_ids.h"
#include "ui/tools/image/lvgl_image.h"
#include "xiaoling/xiaoling_events.h"

#include <utility>
#include <cstring>

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
    presenter->SetStatus(presenter->status_text_);
    presenter->SetTime(presenter->time_text_);
    presenter->SetDuplexMode(presenter->duplex_mode_);
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
    presenter->view_.reset();
    presenter->last_emotion_.clear();
    presenter->last_chat_role_.clear();
    presenter->last_chat_text_.clear();
    presenter->last_voice_text_revision_ = 0;
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
        voice_event_subscriptions_.push_back(events.Subscribe(type, [](const XiaolingEvent& event) {
            UiManager::GetInstance().Dispatch([text = event.text]() {
                if (!text.empty()) {
                    HomePresenter::GetInstance().SetText(text);
                }
            });
        }));
    };
    subscribe_text(XIAOLING_IAT_START);
    subscribe_text(XIAOLING_IAT_UPDATE);
    subscribe_text(XIAOLING_IAT_END);
    subscribe_text(XIAOLING_TTS_TXT_START);
    subscribe_text(XIAOLING_TTS_TXT_UPDATE);
    subscribe_text(XIAOLING_TTS_TXT_END);
}

void HomePresenter::UnsubscribeVoiceEvents() {
    auto& events = XiaolingEventBus::GetInstance();
    for (const auto id : voice_event_subscriptions_) {
        events.Unsubscribe(id);
    }
    voice_event_subscriptions_.clear();
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
    if (view_ != nullptr) {
        view_->SetDuplexMode(mode);
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
    {
        std::lock_guard<std::mutex> lock(banner_mutex_);
        banner_texts_ = std::move(texts);
        banner_index_ = 0;
        has_banners = !banner_texts_.empty();
    }
    if (!has_banners) {
        return;
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
