#pragma once

#include "ui/nav_stack.h"
#include "xiaoling/session/session_core.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cJSON.h>

#include <esp_timer.h>

class HomeView;
class LvglImage;

// Home 页 Presenter：接收外部推送并协调页面 View。
class HomePresenter {
public:
    static HomePresenter& GetInstance();
    static const UiNavScreen& Screen();

    void SetWakeWord(std::string wake_word);
    void HandleSettingMessage(const cJSON* data);
    void SetEmotion(std::string emotion);
    void ShowEmotionOnce(std::string emotion);
    void SetPowerSaveMode(bool enabled);
    void SetText(std::string text);
    void SetChatMessage(std::string role, std::string text);
    void ShowNotification(std::string text, int duration_ms = 3000);
    void SetWifiIcon(std::string icon);
    void SetAlarmActive(bool active);
    void UpdateBattery();
    void SetStatus(std::string status);
    void SetTime(std::string time);
    void SetDuplexMode(DuplexMode mode);
    void SetDuplexVisible(bool visible);
#ifdef CONFIG_LSPLATFORM
    void SetImage(std::unique_ptr<LvglImage> image);
    void ClearImage();
#endif

private:
    struct SubtitleSegment {
        uint32_t cloud_index = 0;
        std::string text;
        uint32_t start_ms = 0;
        uint32_t end_ms = 0;
    };

    HomePresenter() = default;

    static int Open(const UiNavScreen* screen, void** data);
    static int Show(const UiNavScreen* screen, void* data);
    static int Pause(const UiNavScreen* screen, void* data);
    static int Resume(const UiNavScreen* screen, void* data);
    static int Close(const UiNavScreen* screen, void* data);

    void SetBanners(std::vector<std::string> texts, uint32_t interval_ms);
    std::string SubstituteWakeWord(const std::string& text) const;
    void SubscribeVoiceEvents();
    void UnsubscribeVoiceEvents();
    void OnTtsTextStart();
    void OnTtsTextUpdate(const std::string& text);
    void OnTtsTextInterrupt();
    void OnTtsPlaying();
    void OnTtsAudioProgress(uint32_t audio_clock_ms);
    void OnTtsStopped();
    void QueueIatText(std::string text);
    void DrainIatText();
    void QueueTtsAudioProgress(uint32_t audio_clock_ms);
    void ResetSubtitles(bool clear_text);
    void ScheduleSubtitle();
    void StopBannerTimer();
    void OnBannerTick();
    static void BannerTimerCallback(void* arg);

    std::unique_ptr<HomeView> view_;
    std::mutex banner_mutex_;
    std::vector<std::string> banner_texts_;
    size_t banner_index_ = 0;
    esp_timer_handle_t banner_timer_ = nullptr;
    std::vector<uint32_t> voice_event_subscriptions_;
    std::vector<SubtitleSegment> subtitle_segments_;
    size_t subtitle_index_ = 0;
    bool subtitle_audio_started_ = false;
    uint32_t subtitle_audio_clock_ms_ = 0;
    std::atomic<uint32_t> tts_progress_generation_{0};
    std::atomic<uint32_t> latest_tts_audio_clock_ms_{0};
    std::atomic_bool tts_progress_dispatch_pending_{false};
    std::mutex iat_text_mutex_;
    std::string pending_iat_text_;
    int64_t pending_iat_received_us_ = 0;
    int64_t last_iat_delay_log_us_ = 0;
    bool iat_dispatch_pending_ = false;
    bool iat_text_visible_ = false;
    mutable std::mutex wake_word_mutex_;
    std::string wake_word_;
    std::string last_emotion_;
    std::string last_chat_role_;
    std::string last_chat_text_;
    uint32_t last_voice_text_revision_ = 0;
    std::string last_text_;
    std::string wifi_icon_;
    bool alarm_active_ = false;
    int battery_level_ = 0;
    bool battery_charging_ = false;
    std::string status_text_;
    std::string time_text_;
    DuplexMode duplex_mode_ = DuplexMode::kHalfDuplex;
    bool duplex_visible_ = false;
#ifdef CONFIG_LSPLATFORM
    std::shared_ptr<LvglImage> image_;
#endif
};
