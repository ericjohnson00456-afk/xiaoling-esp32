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

#include "xiaoling/conn/xiaoling_conn.h"
#include "ota/ota.h"
#include "audio_service.h"
#include "player/audio_player.h"
#include "player/intents/playback_intent_stack.h"
#include "xiaoling/session/session_core.h"
#include "system/device_status.h"
#include "system/watchdog.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_OTA_CONFIRM_START    (1 << 13)
#define MAIN_EVENT_OTA_CONFIRM_SKIP     (1 << 14)


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
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return DeviceStatus::GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }

    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional localized tone name.
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void Alert(const char* status, const char* message, const char* emotion, uint16_t tone_id);
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    // Thread-safe entry point for board button callbacks. The click business
    // logic is implemented in application_button.cc and runs on the main task.
    void HandleButtonClick(uint8_t click_count);
    void HandleButtonLongPress();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool ResolveKuwoMusicUrl(const std::string& item_id, std::string& url);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    void PlayLocalizedAudio(const char* name);
    void PlayTone(uint16_t tone_id);
    AudioService& GetAudioService() { return audio_service_; }
    AudioPlayer& GetAudioPlayer() { return audio_player_; }
    void SetDuplexMode(DuplexMode mode);
    DuplexMode GetDuplexMode() const { return session_controller_.policy().mode; }

    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetXiaolingConn();

    // Clear saved Wi-Fi credentials and enter the board's provisioning mode.
    // This is also the behavior used by the five-click button shortcut.
    void ClearNetworkConfiguration();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<XiaolingConn> xiaoling_conn_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    AudioPlayer audio_player_;
    VoiceSessionController session_controller_;
    PlaybackIntentStack playback_intents_;
    bool background_music_session_ = false;
    bool background_session_finished_ = false;
    bool music_was_playing_for_session_ = false;
    ListeningMode listening_mode_before_background_session_ = kListeningModeAutoStop;
    std::atomic_bool background_tts_pending_{false};
    uint32_t active_tts_session_id_ = 0;
    // Half-duplex ends listening before the streamed TTS produces its first
    // PCM block. Keep the UI in the thinking state during that idle gap.
    std::atomic_bool tts_waiting_for_playback_{false};
    std::atomic_bool tts_output_started_{false};
    std::atomic_bool exit_requested_{false};
    uint32_t iat_subscription_id_ = 0;
    uint32_t iat_update_subscription_id_ = 0;
    uint32_t iat_end_subscription_id_ = 0;
    bool iat_utterance_active_ = false;
    std::unique_ptr<Ota> ota_;
#ifdef CONFIG_LSPLATFORM
    Watchdog uplink_watchdog_{5000, "uplink"};
    Watchdog downlink_watchdog_{15000, "downlink"};
    std::vector<int32_t> last_watchdog_bites_{};
    bool narrowband_mode_ = false;
#endif // CONFIG_LSPLATFORM

    bool aborted_ = false;
    bool assets_version_checked_ = false;
    std::atomic_bool ota_confirmation_pending_{false};
    std::atomic_int ota_confirmation_decision_{0};
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    std::atomic_bool factory_reset_started_{false};
    // 由 NetworkEvent::Connected/Disconnected 驱动；唤醒时区分“网络未连”和“WS 未连”。
    std::atomic_bool network_connected_{false};
    bool keep_voice_processing_on_idle_ = false;
    int64_t listening_started_at_us_ = 0;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ProcessButtonClick(uint8_t click_count);
    void ProcessButtonLongPress();
    void StartFactoryReset();
    void FactoryResetTask();
    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    bool CheckAssetsVersion();
    bool CheckNewVersion();
    void InitializeXiaolingConn();
    // 唤醒入口的连接检查：WS 就绪返回 true；否则按网络状态播报 085/064 提示音。
    bool EnsureXiaolingReadyForWakeup();
    void ShowActivationCode(const std::string& code, const std::string& message, const std::string& qrcode = "");
    void SetListeningMode(ListeningMode mode);
    void PauseWakeWordDetection();
    bool EndListeningSessionForWakeup();
    void BeginWakeup(ListeningMode mode);
    void FinishBackgroundMusicSession();
    void RestoreBackgroundMusicSession();
    void CancelWakeup();
    void PublishWifiIcon();
    void PlayOtaTone(uint16_t tone_id, uint8_t repeat_count = 1);
    void PlayActivationAudio(uint16_t id);

    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
#ifdef CONFIG_LSPLATFORM
    void CountWatchdogBites();
#endif // CONFIG_LSPLATFORM
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
