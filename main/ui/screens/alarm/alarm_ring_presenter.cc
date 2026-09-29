#include "alarm_ring_presenter.h"

#include "alarm_ring_view.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"

#include <algorithm>
#include <cstdio>

AlarmRingPresenter& AlarmRingPresenter::GetInstance() {
    static AlarmRingPresenter instance;
    return instance;
}

const UiNavScreen& AlarmRingPresenter::Screen() {
    static const UiNavScreen screen = {
        .unique_id = UI_PAGE_ALARM_RING,
        .open = &AlarmRingPresenter::Open,
        .show = &AlarmRingPresenter::Show,
        .pause = &AlarmRingPresenter::Pause,
        .resume = &AlarmRingPresenter::Resume,
        .close = &AlarmRingPresenter::Close,
    };
    return screen;
}

void AlarmRingPresenter::ShowAlarm(const AlarmSnapshot& alarm) {
    alarm_ = alarm;
    auto& manager = UiManager::GetInstance();
    if (manager.TopScreenId() == UI_PAGE_ALARM_RING) {
        Refresh();
    } else {
        manager.NavigateTo(UI_PAGE_ALARM_RING);
    }
}

void AlarmRingPresenter::Dismiss() {
    if (UiManager::GetInstance().TopScreenId() == UI_PAGE_ALARM_RING) {
        UiManager::GetInstance().NavigateHome();
    }
}

int AlarmRingPresenter::Open(const UiNavScreen*, void** data) {
    auto& presenter = GetInstance();
    presenter.view_ = std::make_unique<AlarmRingView>(UiManager::GetInstance().display());
    *data = &presenter;
    return 0;
}

int AlarmRingPresenter::Show(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<AlarmRingPresenter*>(data);
    if (presenter == nullptr || presenter->view_ == nullptr) return -1;
    presenter->view_->Show();
    presenter->Refresh();
    return 0;
}

int AlarmRingPresenter::Pause(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<AlarmRingPresenter*>(data);
    if (presenter != nullptr && presenter->view_ != nullptr) presenter->view_->Hide();
    return 0;
}

int AlarmRingPresenter::Resume(const UiNavScreen* screen, void* data) { return Show(screen, data); }

int AlarmRingPresenter::Close(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<AlarmRingPresenter*>(data);
    if (presenter != nullptr) presenter->view_.reset();
    return 0;
}

void AlarmRingPresenter::Refresh() {
    if (view_ == nullptr) return;
    int year, month, day, hour, minute, second, weekday;
    AlarmService::LocalDateTime(alarm_.next_trigger, year, month, day, hour, minute, second, weekday);
    char time[16] = {};
    char date[32] = {};
    std::snprintf(time, sizeof(time), "%02d:%02d", hour, minute);
    std::snprintf(date, sizeof(date), "%d月%d日", month, day);
    const int remaining = std::max(0, alarm_.spec.snooze_count - alarm_.snooze_used);
    const std::string hint = alarm_.spec.snooze_enabled && remaining > 0
                                 ? "单击: 稍后提醒  长按: 关闭"
                                 : "单击: 关闭闹钟";
    view_->SetContent(time, date, alarm_.spec.text, hint);
}
