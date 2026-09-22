#pragma once

#include <string>

class Display;
struct _lv_obj_t;
using lv_obj_t = _lv_obj_t;
struct _lv_timer_t;
using lv_timer_t = _lv_timer_t;

class AlarmRingView {
public:
    explicit AlarmRingView(Display* display);
    ~AlarmRingView();
    void Show();
    void Hide();
    void SetContent(const std::string& time, const std::string& date,
                    const std::string& text, const std::string& hint);

private:
    Display* display_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* icon_ = nullptr;
    lv_obj_t* time_ = nullptr;
    lv_obj_t* date_ = nullptr;
    lv_obj_t* text_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    lv_timer_t* blink_timer_ = nullptr;
};
