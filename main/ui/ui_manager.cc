#include "ui_manager.h"

#include "display/display.h"
#include "ui/compat/legacy_display_adapter.h"
#include "display/lvgl_display.h"

#include <esp_log.h>

#include <deque>
#include <utility>

#define TAG "UiManager"

UiManager::UiManager() = default;

UiManager::~UiManager() {
    if (display_ != nullptr) {
        display_->SetUiAdapter(nullptr);
    }
}

void UiManager::Initialize(Display* display) {
    if (display == nullptr) {
        ESP_LOGE(TAG, "Initialize: display is NULL");
        return;
    }
    if (display_ != nullptr) {
        display_->SetUiAdapter(nullptr);
    }
    legacy_display_adapter_.reset();
    display_ = display;

    // LCD 的 LVGL 页面使用 lvgl_display_ 完成页面导航；OLED / NoDisplay 转型会失败，
    // 此时仍保留基础 Display 接口，可继续显示通知等兼容内容。
    lvgl_display_ = dynamic_cast<LvglDisplay*>(display);
    if (lvgl_display_ != nullptr) {
        // 旧 Display 接口仍有调用者时，由适配器把这些调用转给新的 UI 页面。
        legacy_display_adapter_ = std::make_unique<LegacyDisplayAdapter>();
        display_->SetUiAdapter(legacy_display_adapter_.get());
    }
    ESP_LOGI(TAG, "UI manager ready, panel %dx%d, lvgl=%s",
             display_->width(), display_->height(), lvgl_display_ != nullptr ? "yes" : "no");
}

bool UiManager::StartTask() {
    // 重复初始化时不再创建第二个 UI 任务。
    if (task_handle_ != nullptr) {
        return true;
    }
    if (xTaskCreate(TaskEntry, "ui", 6144, this, tskIDLE_PRIORITY + 2,
                    &task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI task");
        task_handle_ = nullptr;
        return false;
    }
    // Dispatch() 允许在任务启动前入队，因此启动后主动通知一次，处理已有命令。
    xTaskNotifyGive(task_handle_);
    ESP_LOGI(TAG, "UI task started");
    return true;
}

void UiManager::Dispatch(std::function<void()>&& task) {
    if (!task) {
        return;
    }
    TaskHandle_t task_handle = nullptr;
    {
        // 这里只保护命令队列；具体 UI 操作留到 TaskLoop() 中执行。
        std::lock_guard<std::mutex> lock(task_mutex_);
        tasks_.push_back(std::move(task));
        task_handle = task_handle_;
    }
    if (task_handle != nullptr) {
        // FreeRTOS task notification 仅用于唤醒，不承载命令；命令本体保存在 tasks_。
        xTaskNotifyGive(task_handle);
    }
}

void UiManager::TaskEntry(void* arg) {
    static_cast<UiManager*>(arg)->TaskLoop();
}

void UiManager::TaskLoop() {
    while (true) {
        // 没有 UI 命令时阻塞休眠，不占用 CPU。
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // 一次取走当前全部命令。执行期间不持有队列锁，其他任务仍可继续投递。
        std::deque<std::function<void()>> pending;
        {
            std::lock_guard<std::mutex> lock(task_mutex_);
            pending.swap(tasks_);
        }
        for (auto& task : pending) {
            task();
        }
    }
}

int UiManager::RegisterScreen(const UiNavScreen* scr) {
    return ui_nav_scr_add(scr);
}

int UiManager::SetDefaultScreen(int unique_id) {
    if (lvgl_display_ == nullptr) {
        return -1;
    }
    DisplayLockGuard lock(display_);
    return ui_nav_scr_default_set_by_id(unique_id);
}

int UiManager::NavigateTo(int unique_id) {
    if (lvgl_display_ == nullptr) {
        return -1;
    }
    DisplayLockGuard lock(display_);
    return ui_nav_scr_nav_to(unique_id);
}

int UiManager::NavigateBack() {
    if (lvgl_display_ == nullptr) {
        return -1;
    }
    DisplayLockGuard lock(display_);
    return ui_nav_scr_nav_back();
}

int UiManager::NavigateHome() {
    if (lvgl_display_ == nullptr) {
        return -1;
    }
    DisplayLockGuard lock(display_);
    return ui_nav_scr_nav_default();
}

int UiManager::TopScreenId() {
    if (lvgl_display_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (!lock.locked()) {
            return -1;
        }
        return ui_nav_scr_get_top_id();
    }
    return ui_nav_scr_get_top_id();
}

void UiManager::SetBrightness(uint8_t brightness) {
    if (display_ == nullptr) {
        return;
    }
    display_->SetBrightness(brightness);
}

uint8_t UiManager::GetBrightness() {
    if (display_ == nullptr) {
        return 0;
    }
    return display_->GetBrightness();
}
