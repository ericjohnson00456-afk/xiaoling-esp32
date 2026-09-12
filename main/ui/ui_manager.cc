#include "ui_manager.h"

#include "display.h"
#include "home_presenter.h"
#include "lvgl_display.h"

#include <esp_log.h>

#include <utility>

#define TAG "UiManager"

UiManager::UiManager() = default;

UiManager::~UiManager() = default;

void UiManager::Initialize(Display* display) {
    if (display == nullptr) {
        ESP_LOGE(TAG, "Initialize: display is NULL");
        return;
    }
    display_ = display;
    // OLED / NoDisplay 场景下转型失败，仅页面导航不可用，banner 等通用能力不受影响
    lvgl_display_ = dynamic_cast<LvglDisplay*>(display);
    ESP_LOGI(TAG, "UI manager ready, panel %dx%d, lvgl=%s",
             display_->width(), display_->height(), lvgl_display_ != nullptr ? "yes" : "no");
}

void UiManager::SetTaskScheduler(std::function<void(std::function<void()>&&)> scheduler) {
    task_scheduler_ = std::move(scheduler);
}

void UiManager::Dispatch(std::function<void()>&& task) {
    if (task_scheduler_) {
        task_scheduler_(std::move(task));
    } else if (task) {
        task();
    }
}

int UiManager::RegisterScreen(const UiNavScreen* scr) {
    return ui_nav_scr_add(scr);
}

int UiManager::SetDefaultScreen(int unique_id) {
    if (lvgl_display_ == nullptr) {
        return -1;
    }
    DisplayLockGuard lock(display_);
    return ui_nav_scr_default_set_by_id(unique_id);
}

int UiManager::NavigateTo(int unique_id) {
    if (lvgl_display_ == nullptr) {
        return -1;
    }
    DisplayLockGuard lock(display_);
    return ui_nav_scr_nav_to(unique_id);
}

int UiManager::NavigateBack() {
    if (lvgl_display_ == nullptr) {
        return -1;
    }
    DisplayLockGuard lock(display_);
    return ui_nav_scr_nav_back();
}

int UiManager::NavigateHome() {
    if (lvgl_display_ == nullptr) {
        return -1;
    }
    DisplayLockGuard lock(display_);
    return ui_nav_scr_nav_default();
}

int UiManager::TopScreenId() {
    return ui_nav_scr_get_top_id();
}

void UiManager::SetBrightness(uint8_t brightness) {
    if (display_ == nullptr) {
        return;
    }
    display_->SetBrightness(brightness);
}

uint8_t UiManager::GetBrightness() {
    if (display_ == nullptr) {
        return 0;
    }
    return display_->GetBrightness();
}

void UiManager::SetBannerIdleProvider(std::function<bool()> provider) {
    HomePresenter::GetInstance().SetBannerIdleProvider(std::move(provider));
}
