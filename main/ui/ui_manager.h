#pragma once

#include "ui/nav_stack.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Display;
class LvglDisplay;
class LegacyDisplayAdapter;

/*
 * UI 层入口：display 层只负责面板/LVGL 硬件初始化与亮度控制，
 * 页面组织（导航栈、页面注册与切换）由 UiManager 承担。
 *
 * 初始化链路：
 *   Ui::Initialize()
 *     -> Initialize(display)       绑定显示设备
 *     -> RegisterScreen(...)       注册页面
 *     -> SetDefaultScreen(...)     指定首页
 *     -> StartTask()               启动 UI 命令任务
 *
 * 运行链路：业务任务 -> Ui::XXX() -> Dispatch() -> FIFO 队列 -> UI 任务。
 *
 * 导航操作内部都会持有 display lock，页面回调里可以直接操作 LVGL 对象。
 */
class UiManager {
public:
    static UiManager& GetInstance() {
        static UiManager instance;
        return instance;
    }

    /* 绑定 display 层创建好的显示设备。 */
    void Initialize(Display* display);

    /* 页面注册完成后启动 UI 命令任务。 */
    bool StartTask();

    /*
     * 将 UI 更新放入队列，所有命令由 UI 专用任务按先进先出顺序执行。
     * 即使任务尚未启动，命令也会暂存在队列中，StartTask() 后统一处理。
     */
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

private:
    UiManager();
    ~UiManager();
    UiManager(const UiManager&) = delete;
    UiManager& operator=(const UiManager&) = delete;

    static void TaskEntry(void* arg);  // FreeRTOS 的 C 风格任务入口。
    void TaskLoop();                   // 真正的 UI 命令消费循环。

    Display* display_ = nullptr;
    LvglDisplay* lvgl_display_ = nullptr;
    std::unique_ptr<LegacyDisplayAdapter> legacy_display_adapter_;
    TaskHandle_t task_handle_ = nullptr;
    std::mutex task_mutex_;
    std::deque<std::function<void()>> tasks_;
};
