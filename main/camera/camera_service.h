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

// 拍照业务状态机：协调预览采帧、拍照提示音、云端请求、TTS 播放以及
// 回到 Home 后的图片和表情清理。未来语音/MCP 拍照可复用该流程，改为
// 预览三秒后自动触发，并替换云端结果提交策略。耗时采集在独立任务中执行。
class CameraService {
  public:
    static CameraService& GetInstance();

    void Initialize(Application& application);
    bool StartButtonCapture();
    bool Capture();
    bool Exit();
    bool IsActive() const;

  private:
    enum class Origin : uint8_t {
        Button,
        VoiceMcp,
    };

    enum class Phase : uint8_t {
        Inactive,   // 未进入拍照流程
        Starting,   // Intent 正在抢占资源，尚未创建预览任务
        Preview,    // 实时预览并等待单击
        Processing, // 已冻结照片，等待提示音或图片编码
        WaitingTts, // 图片已通过 WS 提交，等待云端 TTS
        Speaking,   // 正在播放图片识别结果
        Exiting,    // 等待当前采帧安全结束后退出
    };

    struct TaskContext {
        CameraService* service = nullptr;
        uint32_t generation = 0;
        bool stack_in_psram = false;
    };

    CameraService() = default;
    ~CameraService();

    bool Start(Origin origin, uint32_t auto_capture_delay_ms);
    static void TaskEntry(void* arg);
    void TaskLoop(uint32_t generation);
    void RequestCloudDescription(uint32_t generation, Camera& camera);
    void HandlePlaybackEvent(const PlaybackEvent& event);
    void HandleIntentPreempted();
    void Fail(uint32_t generation, const char* message);
    void FinishExit(bool stop_audio);
    bool IsCurrent(uint32_t generation) const;

    Application* application_ = nullptr;
    mutable std::mutex mutex_;
    std::atomic<Phase> phase_{Phase::Inactive};
    std::atomic_uint32_t generation_{0};
    std::atomic_bool capture_requested_{false};
    std::atomic_bool exit_requested_{false};
    std::atomic_bool preempted_exit_{false};
    TaskHandle_t task_handle_ = nullptr;
    uint32_t tts_session_id_ = 0;
    uint32_t capture_tone_session_id_ = 0;
    std::atomic_bool capture_tone_finished_{false};
    Origin origin_ = Origin::Button;
    // 按键模式为 0，等待用户单击；未来语音/MCP 模式设置为 3000 后自动拍照。
    uint32_t auto_capture_delay_ms_ = 0;
    std::vector<AppEvent::SubscriptionId> subscriptions_;
};
