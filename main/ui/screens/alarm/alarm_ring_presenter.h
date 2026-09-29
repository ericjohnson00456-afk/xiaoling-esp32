#pragma once

#include "alarm/alarm_service.h"
#include "ui/nav_stack.h"

#include <memory>

class AlarmRingView;

class AlarmRingPresenter {
public:
    static AlarmRingPresenter& GetInstance();
    static const UiNavScreen& Screen();

    // 由 ui.cc 在 UI 任务中调用。
    void ShowAlarm(const AlarmSnapshot& alarm);
    void Dismiss();

private:
    static int Open(const UiNavScreen*, void** data);
    static int Show(const UiNavScreen*, void* data);
    static int Pause(const UiNavScreen*, void* data);
    static int Resume(const UiNavScreen*, void* data);
    static int Close(const UiNavScreen*, void* data);
    void Refresh();

    std::unique_ptr<AlarmRingView> view_;
    AlarmSnapshot alarm_;
};
