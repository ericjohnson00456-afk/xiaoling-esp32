#pragma once

#include <lvgl.h>

#include <string>

// Reusable transient message overlay. The widget is attached to LVGL's top
// layer, so it remains visible while navigation switches pages underneath it.
class Toast {
public:
    static Toast& GetInstance();

    void Show(const std::string& text, int duration_ms = 3000);
    void Dismiss();

private:
    Toast() = default;

    bool EnsureCreated();
    static void TimerCallback(lv_timer_t* timer);

    lv_obj_t* container_ = nullptr;
    lv_obj_t* label_ = nullptr;
    lv_timer_t* timer_ = nullptr;
};
