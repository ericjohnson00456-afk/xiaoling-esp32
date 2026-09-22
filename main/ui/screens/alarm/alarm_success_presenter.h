#pragma once

#include "alarm/alarm_service.h"
#include "ui/nav_stack.h"

#include <memory>

class AlarmSuccessView;

class AlarmSuccessPresenter {
public:
    static AlarmSuccessPresenter& GetInstance();
    static const UiNavScreen& Screen();

    void ShowAlarm(const AlarmSnapshot& alarm, bool newly_created = false);

private:
    static int Open(const UiNavScreen*, void** data);
    static int Show(const UiNavScreen*, void* data);
    static int Pause(const UiNavScreen*, void* data);
    static int Resume(const UiNavScreen*, void* data);
    static int Close(const UiNavScreen*, void* data);
    static void TimeoutCallback(void* timer);

    void Refresh();
    void StartTimeout();
    void StopTimeout();

    std::unique_ptr<AlarmSuccessView> view_;
    AlarmSnapshot alarm_;
    void* timeout_timer_ = nullptr;
};
