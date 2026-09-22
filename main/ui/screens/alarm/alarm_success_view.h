#pragma once

#include <string>

class Display;
struct _lv_obj_t;
using lv_obj_t = _lv_obj_t;

class AlarmSuccessView {
public:
    explicit AlarmSuccessView(Display* display);
    ~AlarmSuccessView();
    void Show();
    void Hide();
    void SetContent(const std::string& time, const std::string& date);

private:
    Display* display_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* time_ = nullptr;
    lv_obj_t* date_ = nullptr;
};
