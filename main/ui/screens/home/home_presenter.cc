#include "home_presenter.h"

#include "banner_model.h"
#include "display.h"
#include "home_view.h"
#include "status_model.h"
#include "ui/core/ui_manager.h"
#include "ui/core/ui_page_ids.h"
#include "voice_model.h"
#include "wifi_model.h"

#include <utility>

#include <esp_log.h>

#define TAG "HomePresenter"

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

int HomePresenter::Open(const UiNavScreen* screen, void** data) {
    (void)screen;
    auto& presenter = GetInstance();
    presenter.view_ = std::make_unique<HomeView>(UiManager::GetInstance().display());
    presenter.SubscribeModels();
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
    presenter->UnsubscribeModels();
    presenter->StopBannerTimer();
    presenter->view_.reset();
    presenter->last_emotion_.clear();
    presenter->last_voice_text_revision_ = 0;
    return 0;
}

void HomePresenter::SubscribeModels() {
    voice_subscription_ = VoiceModel::GetInstance().Subscribe([](const VoiceModelState& state) {
        UiManager::GetInstance().Dispatch([state]() {
            auto& home = HomePresenter::GetInstance();
            if (home.view_ == nullptr) {
                return;
            }
            if (home.last_emotion_ != state.emotion) {
                home.last_emotion_ = state.emotion;
                home.view_->SetEmotion(state.emotion);
            }
            if (!state.text.empty() &&
                home.last_voice_text_revision_ != state.text_revision) {
                home.last_voice_text_revision_ = state.text_revision;
#ifdef CONFIG_LSPLATFORM
                if (state.text_source == VoiceTextSource::kIat) {
                    auto* display = UiManager::GetInstance().display();
                    display->DismissImage();
                    display->DismissActivation();
                }
#endif
                home.view_->SetText(state.text);
            }
        });
    });

    banner_subscription_ = BannerModel::GetInstance().Subscribe([](const BannerModelState& state) {
        auto& home = HomePresenter::GetInstance();
        home.SetBanners(state.texts, state.interval_ms);
    });

    wifi_subscription_ = WifiModel::GetInstance().Subscribe([](const WifiModelState& state) {
        UiManager::GetInstance().Dispatch([state]() {
            auto& home = HomePresenter::GetInstance();
            if (home.view_ != nullptr) {
                home.view_->SetWifiIcon(state.icon);
            }
        });
    });

    status_subscription_ = StatusModel::GetInstance().Subscribe([](const StatusModelState& state) {
        UiManager::GetInstance().Dispatch([state]() {
            auto& home = HomePresenter::GetInstance();
            if (home.view_ != nullptr) {
                home.view_->SetStatus(state.text);
            }
        });
    });

}

void HomePresenter::UnsubscribeModels() {
    VoiceModel::GetInstance().Unsubscribe(voice_subscription_);
    BannerModel::GetInstance().Unsubscribe(banner_subscription_);
    WifiModel::GetInstance().Unsubscribe(wifi_subscription_);
    StatusModel::GetInstance().Unsubscribe(status_subscription_);
    voice_subscription_ = 0;
    banner_subscription_ = 0;
    wifi_subscription_ = 0;
    status_subscription_ = 0;
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
