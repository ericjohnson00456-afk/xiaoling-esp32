#pragma once

#include <lvgl.h>

#include <string>

class Display;

class UpgradeView {
public:
    explicit UpgradeView(Display* display);
    ~UpgradeView();

    void Show();
    void Hide();
    void SetProgress(const std::string& target, int progress, size_t speed);
    void SetResult(const std::string& message);

private:
    void CreateUi();
    void DestroyUi();

    Display* display_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* target_ = nullptr;
    lv_obj_t* bar_ = nullptr;
    lv_obj_t* percent_ = nullptr;
    lv_obj_t* speed_ = nullptr;
};
