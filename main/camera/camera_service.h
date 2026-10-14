#pragma once

#include "event/app_event.h"
#include "player/types/playback_event.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class Application;
class Camera;

// 拍照业务状态机：按键拍照等待用户单击并通过 WS 识图；语音/MCP 拍照
// 预览三秒后自动采集，通过 HTTP 上传并异步返回图片 URL。预览采帧、提示音、
// TTS 衔接以及 Home 图片清理都在这里协调，耗时操作放在独立任务中执行。
class CameraService {
  public:
    using VoiceMcpResultCallback =
        std::function<void(bool success, std::string value)>;

    static CameraService& GetInstance();

    void Initialize(Application& application);
    bool StartButtonCapture();
    // MCP 拍照异步返回图片 URL；失败或取消时 value 为错误说明。
    bool StartVoiceMcpCapture(bool sync, VoiceMcpResultCallback callback);
    // 按键或唤醒词只在三秒预览倒计时阶段取消语音拍照。
    bool CancelVoiceMcpCountdown(const char* reason);
    // 提前到达的拍照 TTS URL 要等 MCP 响应发出后才交给播放器。
    bool DeferVoiceMcpTtsUrl(const std::string& url, bool pushup = false);
    // 按键拍照拥有独立的 TTS 收尾；语音拍照沿用普通语音会话策略。
    bool OwnsStandaloneTtsLifecycle() const;
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
        WaitingTts, // 图片已提交且工具已响应，等待云端 TTS
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

    bool Start(Origin origin, uint32_t auto_capture_delay_ms,
               VoiceMcpResultCallback callback = {},
               std::string* error = nullptr);
    static void TaskEntry(void* arg);
    void TaskLoop(uint32_t generation);
    void RequestCloudDescription(uint32_t generation, Camera& camera);
    void UploadMcpPhoto(uint32_t generation, Camera& camera);
    void CompleteVoiceMcp(uint32_t generation, bool success,
                          std::string value, bool keep_until_tts);
    void ReleaseVoiceMcpTtsGate(bool replay);
    void ReleaseCameraIntent();
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
    std::atomic_bool camera_intent_active_{false};
    TaskHandle_t task_handle_ = nullptr;
    uint32_t tts_session_id_ = 0;
    uint32_t capture_tone_session_id_ = 0;
    std::atomic_bool capture_tone_finished_{false};
    std::atomic<Origin> origin_{Origin::Button};
    // 按键模式为 0，等待用户单击；语音/MCP 模式设置为 3000 后自动拍照。
    uint32_t auto_capture_delay_ms_ = 0;
    VoiceMcpResultCallback voice_mcp_result_callback_;
    bool voice_tts_gate_active_ = false;
    bool voice_mcp_sync_ = false;
    bool voice_mcp_result_sent_ = false;
    std::string deferred_voice_tts_url_;
    bool deferred_voice_tts_pushup_ = false;
    // 云端可能先下发 TTS URL、数秒后才下发 take_photo；保留最近候选供接管。
    std::string recent_tts_url_;
    bool recent_tts_pushup_ = false;
    int64_t recent_tts_url_at_us_ = 0;
    std::vector<AppEvent::SubscriptionId> subscriptions_;
};
