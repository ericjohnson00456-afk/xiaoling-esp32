#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <cstdint>
#include <atomic>
#include <vector>

#include "xiaoling/conn/xiaoling_conn.h"
#include "ota/ota.h"
#include "audio_service.h"
#include "player/audio_player.h"
#include "player/intents/playback_intent_manager.h"
#include "xiaoling/session/session_core.h"
#include "system/device_status.h"
#include "system/watchdog.h"

// 主事件位
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_OTA_COMPLETED        (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 禁止复制 Application 单例。
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * 初始化应用程序。
     * 配置显示、音频和网络回调等模块，并异步启动网络连接。
     */
    void Initialize();

    /**
     * 运行主事件循环。
     * 本方法在主任务中持续运行，负责处理网络、状态变化和用户交互等事件。
     */
    void Run();

    /**
     * 读取设备当前的全局运行状态。
     * 供板型、显示和 LED 等模块使用
     */
    DeviceState GetDeviceState() const { return DeviceStatus::GetState(); }

    /**
     * 请求切换设备状态。
     * 切换成功返回 true。
     */
    bool SetDeviceState(DeviceState state) { return DeviceStatus::TransitionTo(state); }

    /**
     * 返回音频处理器的 VAD 检测结果
     * 用于监听状态下的人声 LED 指示。
     */
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }

    /**
     * 将回调投递到 Application 主任务执行。
     */
    void Schedule(std::function<void()>&& callback);
    void ScheduleUrgent(std::function<void()>&& callback);

    /**
     * 显示包含状态、消息和表情的警告，可选择播放本地化提示音。
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void Alert(const char* status, const char* message, const char* emotion, uint16_t tone_id);
    void DismissAlert();

    /**
     * 根据原因中止当前语音交互。
     * 唤醒词会停止当前 TTS 并上报打断位置；退出指令只结束上行并允许 TTS 播放完成。
     * 本方法会同步操作语音会话和播放器，应在 Application 主任务中调用。
     */
    void AbortSpeaking(AbortReason reason);

    /**
     * 以事件方式切换对话状态，可跨任务安全调用。
     * 实际处理由主循环中的 MAIN_EVENT_TOGGLE_CHAT 事件完成。
     */
    void ToggleChatState();

    /**
     * 以事件方式开始监听，可跨任务安全调用。
     * 实际处理由主循环中的 MAIN_EVENT_START_LISTENING 事件完成。
     */
    void StartListening();

    /**
     * 以事件方式停止监听，可跨任务安全调用。
     * 实际处理由主循环中的 MAIN_EVENT_STOP_LISTENING 事件完成。
     */
    void StopListening();

    /*  ========== 按键业务 ========*/
    // 按键业务的板级入口。两个方法均可从按键回调任务安全调用，
    // 实际处理会投递到 Application 主任务，实现见 application_button.cc。
    // 根据连击次数投递一次短击事件。
    void HandleButtonClick(uint8_t click_count);
    // 投递一次长按事件。
    void HandleButtonLongPress();
    // 清除 Wi-Fi 配置、重置小聆连接并进入配网状态；五击和串口 wifi 命令共用。
    void ClearNetworkConfiguration();

    void WakeWordInvoke(const std::string& wake_word);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlayRespakAudio(const char* name);
    void PlayTone(uint16_t tone_id);
    /*  ========== 闹钟业务 ======== */
    // AlarmService 通过以下接口使用 Application 的播放器和小聆连接。
    uint32_t StartAlarmTone(uint16_t tone_id);
    uint32_t StartAlarmTtsUrl(const std::string& url);
    bool RequestAlarmTtsUrl(const std::string& text);
    void CancelAlarmTtsRequest();
    bool FetchCloudAlarmList(std::string& response_body);
    bool ResolveCustomAlarmDate(const std::string& alarm_id, std::string& date);
    void StopAlarmTone();

    AudioService& GetAudioService() { return audio_service_; }
    AudioPlayer& GetAudioPlayer() { return audio_player_; }
    PlaybackIntentManager& GetIntentManager() { return intent_manager_; }
    void SetDuplexMode(DuplexMode mode);
    DuplexMode GetDuplexMode() const { return session_controller_.policy().mode; }

    /**
     * 重置协议资源，可跨任务安全调用。
     * 释放联网后创建的资源，包括关闭音频通道以及重置协议和 OTA 对象。
     */
    void ResetXiaolingConn();

private:
    friend class MusicIntent;
    friend class VoiceSessionIntent;

    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_urgent_tasks_;
    std::deque<std::function<void()>> main_tasks_;
    std::mutex xiaoling_conn_mutex_;
    std::unique_ptr<XiaolingConn> xiaoling_conn_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    AudioPlayer audio_player_;
    VoiceSessionController session_controller_;
    PlaybackIntentManager intent_manager_;
    uint32_t active_tts_session_id_ = 0;
    uint32_t pending_tts_session_id_ = 0;
    // 半双工模式会在流式 TTS 产生首个 PCM 数据块前结束监听；
    // 在这段空档期保持界面处于“思考中”状态。
    std::atomic_bool tts_waiting_for_playback_{false};
    std::atomic_bool tts_output_started_{false};
    /* Incremented synchronously when each TTS URL is admitted. Delayed IAT
       work may only interrupt the TTS generation present when it arrived. */
    std::atomic_uint32_t tts_url_generation_{0};
    uint32_t interrupted_tts_generation_ = 0;
    std::atomic_bool exit_requested_{false};
    uint32_t iat_subscription_id_ = 0;
    uint32_t iat_update_subscription_id_ = 0;
    uint32_t iat_end_subscription_id_ = 0;
    bool iat_utterance_active_ = false;

    /*  ========== OTA业务 ======== */
    // OTA 流程独立于设备激活和小聆连接。application_ota.cc 只负责
    // 固件、资源检查与更新，并通过 MAIN_EVENT_OTA_COMPLETED 报告结果。
    struct OtaFlowState {
        std::unique_ptr<Ota> client;
        TaskHandle_t task_handle = nullptr;
        bool resources_checked = false;
        std::atomic_bool completed{false};
        std::atomic_bool succeeded{false};
        std::atomic_bool confirmation_pending{false};
        std::atomic_int confirmation_decision{0};
    } ota_flow_;

    void StartOtaFlow();
    void OtaTask();
    void CompleteOtaFlow(bool succeeded);
    bool HandleOtaConfirmation(bool confirm);
    bool CheckAssetsVersion();
    bool CheckNewVersion();
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    void PlayOtaTone(uint16_t tone_id, uint8_t repeat_count = 1);

    // 设备激活与小聆连接属于 Application 启动阶段，不是 OTA 的一部分。
    bool WaitForOtaFlowCompletion();
    void RunXiaolingActivation();
    void ShowActivationCode(const std::string& code, const std::string& message,
                            const std::string& qrcode = "");
    void PlayActivationAudio(uint16_t id);

    /*  ========== 闹钟业务 ======== */
    // application_alarm.cc 通过这个唯一入口安装播放、时钟、唤醒词和小聆连接钩子。
    void InitializeAlarm();
    struct AlarmFlowState {
        ~AlarmFlowState();

        bool initialized = false;
        uint32_t connected_subscription_id = 0;
        std::function<void()> on_clock_tick;
        std::function<void()> on_wake_word;
    } alarm_flow_;

#ifdef CONFIG_LSPLATFORM
    Watchdog uplink_watchdog_{5000, "uplink"};
    Watchdog downlink_watchdog_{15000, "downlink"};
    std::vector<int32_t> last_watchdog_bites_{};
    bool narrowband_mode_ = false;
#endif  // 小聆平台

    bool aborted_ = false;

    int clock_ticks_ = 0;
    // 由 NetworkEvent::Connected/Disconnected 驱动；唤醒时区分“网络未连”和“WS 未连”。
    std::atomic_bool network_connected_{false};
    bool keep_voice_processing_on_idle_ = false;
    int64_t listening_started_at_us_ = 0;


    // 主事件处理函数。
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleWakeWordDetectedEvent();
    void ProcessMainEvents(EventBits_t bits);
    void InitializeXiaolingConn();
    // 唤醒入口的连接检查：WS 就绪返回 true；否则按网络状态播报 085/064 提示音。
    bool EnsureXiaolingReadyForWakeup();
    void SetListeningMode(ListeningMode mode);
    void PauseWakeWordDetection();
    bool EndListeningSessionForWakeup();
    void BeginWakeup(ListeningMode mode);
    void CancelWakeup();
    void PublishWifiIcon();
    void LogUplinkDiagnostics(const char* phase);

    /*  ========== 按键业务 ======== */
    // 按键业务实现。application_button.cc 负责短击、长按、重新配网和恢复出厂。
    // 在 Application 主任务中执行短击次数对应的业务。
    void ProcessButtonClick(uint8_t click_count);
    // 在 Application 主任务中处理长按，用于停止闹钟或跳过 OTA。
    void ProcessButtonLongPress();
    // 启动恢复出厂任务。
    void StartFactoryReset();
    // 播放提示音后执行恢复出厂和重启。
    void FactoryResetTask();

#ifdef CONFIG_LSPLATFORM
    void CountWatchdogBites();
#endif  // 小聆平台
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
