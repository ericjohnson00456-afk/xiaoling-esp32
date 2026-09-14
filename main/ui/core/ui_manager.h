#pragma once

#include "ui/core/nav_stack.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Display;
class LvglDisplay;

/*
 * UI 层入口：display 层只负责面板/LVGL 硬件初始化与亮度控制，
 * 页面组织（导航栈、页面注册与切换）由 UiManager 承担。
 *
 * 导航操作内部都会持有 display lock，页面回调里可以直接操作 LVGL 对象。
 */
class UiManager {
public:
    static UiManager& GetInstance() {
        static UiManager instance;
        return instance;
    }

    /* 绑定 display 层创建好的显示设备（application 在板级初始化完成后调用） */
    void Initialize(Display* display);
    void SetTaskScheduler(std::function<void(std::function<void()>&&)> scheduler);
    void Dispatch(std::function<void()>&& task);

    bool initialized() const { return display_ != nullptr; }
    Display* display() const { return display_; }
    /* 仅当底层是 LVGL 显示设备时非空，页面/导航需要用它挂 lv_display_t */
    LvglDisplay* lvgl_display() const { return lvgl_display_; }

    /* 页面导航（带 display lock 包装 nav_stack） */
    int RegisterScreen(const UiNavScreen* scr);
    int SetDefaultScreen(int unique_id);
    int NavigateTo(int unique_id);
    int NavigateBack();
    int NavigateHome();
    int TopScreenId();

    /* 透传 display 层的亮度控制（0-100） */
    void SetBrightness(uint8_t brightness);
    uint8_t GetBrightness();

    /* Banner 是否允许轮换，由 application 注入当前设备空闲状态。 */
    void SetBannerIdleProvider(std::function<bool()> provider);

private:
    UiManager();
    ~UiManager();
    UiManager(const UiManager&) = delete;
    UiManager& operator=(const UiManager&) = delete;

    Display* display_ = nullptr;
    LvglDisplay* lvgl_display_ = nullptr;
    std::function<void(std::function<void()>&&)> task_scheduler_;
};
