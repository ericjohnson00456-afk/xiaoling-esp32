#include "alarm_success_presenter.h"

#include "alarm_success_view.h"
#include "ui/ui_app.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"

#include <cstdio>
#include <lvgl.h>

AlarmSuccessPresenter& AlarmSuccessPresenter::GetInstance() {
    static AlarmSuccessPresenter instance;
    return instance;
}

const UiNavScreen& AlarmSuccessPresenter::Screen() {
    static const UiNavScreen screen = {
        .unique_id = UI_PAGE_ALARM_SUCCESS,
        .open = &AlarmSuccessPresenter::Open,
        .show = &AlarmSuccessPresenter::Show,
        .pause = &AlarmSuccessPresenter::Pause,
        .resume = &AlarmSuccessPresenter::Resume,
        .close = &AlarmSuccessPresenter::Close,
    };
    return screen;
}

void AlarmSuccessPresenter::ShowAlarm(const AlarmSnapshot& alarm, bool newly_created) {
    UiManager::GetInstance().Dispatch([alarm, newly_created]() {
        auto& presenter = GetInstance();
        presenter.alarm_ = alarm;
        if (UiManager::GetInstance().lvgl_display() == nullptr) {
            UiApp::ShowNotification("闹钟设置成功", 3000);
            return;
        }
        if (UiManager::GetInstance().TopScreenId() == UI_PAGE_ALARM_RING) {
            if (newly_created) {
                UiApp::ShowToast("新闹钟创建成功", 3000);
            } else {
                UiApp::ShowNotification("闹钟设置成功", 3000);
            }
            return;
        }
        UiManager::GetInstance().NavigateTo(UI_PAGE_ALARM_SUCCESS);
        presenter.Refresh();
        presenter.StartTimeout();
    });
}

int AlarmSuccessPresenter::Open(const UiNavScreen*, void** data) {
    auto& presenter = GetInstance();
    presenter.view_ = std::make_unique<AlarmSuccessView>(UiManager::GetInstance().display());
    *data = &presenter;
    return 0;
}

int AlarmSuccessPresenter::Show(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<AlarmSuccessPresenter*>(data);
    if (presenter == nullptr || presenter->view_ == nullptr) return -1;
    presenter->view_->Show();
    presenter->Refresh();
    presenter->StartTimeout();
    return 0;
}

int AlarmSuccessPresenter::Pause(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<AlarmSuccessPresenter*>(data);
    if (presenter != nullptr) {
        presenter->StopTimeout();
        if (presenter->view_ != nullptr) presenter->view_->Hide();
    }
    return 0;
}

int AlarmSuccessPresenter::Resume(const UiNavScreen* screen, void* data) {
    return Show(screen, data);
}

int AlarmSuccessPresenter::Close(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<AlarmSuccessPresenter*>(data);
    if (presenter != nullptr) {
        presenter->StopTimeout();
        presenter->view_.reset();
    }
    return 0;
}

void AlarmSuccessPresenter::Refresh() {
    if (view_ == nullptr || alarm_.next_trigger == 0) return;
    int year, month, day, hour, minute, second, weekday;
    AlarmService::LocalDateTime(alarm_.next_trigger, year, month, day, hour, minute, second, weekday);
    static constexpr const char* kWeekdays[] = {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    char time[16] = {};
    char date[48] = {};
    std::snprintf(time, sizeof(time), "%02d:%02d", hour, minute);
    std::snprintf(date, sizeof(date), "%d月%d日 %s", month, day, kWeekdays[weekday]);
    view_->SetContent(time, date);
}

void AlarmSuccessPresenter::StartTimeout() {
    StopTimeout();
    auto* timer = lv_timer_create([](lv_timer_t* value) {
        TimeoutCallback(value);
    }, 3000, this);
    if (timer != nullptr) {
        lv_timer_set_repeat_count(timer, 1);
        timeout_timer_ = timer;
    }
}

void AlarmSuccessPresenter::StopTimeout() {
    if (timeout_timer_ != nullptr) {
        lv_timer_delete(static_cast<lv_timer_t*>(timeout_timer_));
        timeout_timer_ = nullptr;
    }
}

void AlarmSuccessPresenter::TimeoutCallback(void* timer_ptr) {
    auto* timer = static_cast<lv_timer_t*>(timer_ptr);
    auto* presenter = timer != nullptr ? static_cast<AlarmSuccessPresenter*>(lv_timer_get_user_data(timer)) : nullptr;
    if (presenter != nullptr) presenter->timeout_timer_ = nullptr;
    UiManager::GetInstance().Dispatch([]() { UiManager::GetInstance().NavigateHome(); });
}
