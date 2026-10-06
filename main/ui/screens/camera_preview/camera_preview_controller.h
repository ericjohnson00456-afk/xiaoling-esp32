#pragma once

#include "event/app_event.h"
#include "player/types/playback_event.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class Application;
class Camera;

// 按键拍照业务状态机：协调预览采帧、拍照提示音、WS 图片识别、TTS 播放
// 以及回到 Home 后的图片和表情清理。耗时采集在独立任务中执行。
class CameraPreviewController {
  public:
    static CameraPreviewController& GetInstance();

    void Initialize(Application& application);
    bool Enter();
    bool Capture();
    bool Exit();
    bool IsActive() const;

  private:
    enum class Phase : uint8_t {
        Inactive,   // 未进入拍照流程
        Preview,    // 实时预览并等待单击
        Processing, // 已冻结照片，等待提示音或图片编码
        WaitingTts, // 图片已通过 WS 提交，等待云端 TTS
        Speaking,   // 正在播放图片识别结果
        Exiting,    // 等待当前采帧安全结束后退出
    };

    struct TaskContext {
        CameraPreviewController* controller = nullptr;
        uint32_t generation = 0;
        bool stack_in_psram = false;
    };

    CameraPreviewController() = default;
    ~CameraPreviewController();

    static void TaskEntry(void* arg);
    void TaskLoop(uint32_t generation);
    void RequestCloudDescription(uint32_t generation, Camera& camera);
    void HandlePlaybackEvent(const PlaybackEvent& event);
    void Fail(uint32_t generation, const char* message);
    void FinishExit(bool stop_audio);
    bool IsCurrent(uint32_t generation) const;

    Application* application_ = nullptr;
    mutable std::mutex mutex_;
    std::atomic<Phase> phase_{Phase::Inactive};
    std::atomic_uint32_t generation_{0};
    std::atomic_bool capture_requested_{false};
    std::atomic_bool exit_requested_{false};
    TaskHandle_t task_handle_ = nullptr;
    uint32_t tts_session_id_ = 0;
    uint32_t capture_tone_session_id_ = 0;
    std::atomic_bool capture_tone_finished_{false};
    bool resume_music_ = false;
    std::vector<AppEvent::SubscriptionId> subscriptions_;
};
