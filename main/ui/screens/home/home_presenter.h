#pragma once

#include "ui/nav_stack.h"
#include "voice_subtitle_controller.h"
#include "xiaoling/session/session_core.h"
#include "player/types/music_track.h"

#include <cstdint>
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
    void SetSubtitleText(std::string text, uint8_t max_lines = 3);
    void SetChatMessage(std::string role, std::string text);
    void ShowNotification(std::string text, int duration_ms = 3000);
    void SetWifiIcon(std::string icon);
    void SetTfCardPresent(bool present);
    void SetAlarmActive(bool active);
    void UpdateBattery();
    void SetStatus(std::string status, bool standby = false);
    void SetMusicTrack(MusicTrack track);
    void SetTime(std::string time);
    void SetDuplexMode(DuplexMode mode);
    void SetDuplexVisible(bool visible);
#ifdef CONFIG_LSPLATFORM
    void SetImage(std::unique_ptr<LvglImage> image);
    void ClearImage();
#endif

private:
    friend class VoiceSubtitleController;

    HomePresenter();

    static int Open(const UiNavScreen* screen, void** data);
    static int Show(const UiNavScreen* screen, void* data);
    static int Pause(const UiNavScreen* screen, void* data);
    static int Resume(const UiNavScreen* screen, void* data);
    static int Close(const UiNavScreen* screen, void* data);

    void SetBanners(std::vector<std::string> texts, uint32_t interval_ms);
    std::string SubstituteWakeWord(const std::string& text) const;
    void StopBannerTimer();
    void OnBannerTick();
    void RenderMusicTrack();
    static void BannerTimerCallback(void* arg);

    std::unique_ptr<HomeView> view_;
    VoiceSubtitleController voice_subtitles_;
    std::mutex banner_mutex_;
    std::vector<std::string> banner_texts_;
    size_t banner_index_ = 0;
    esp_timer_handle_t banner_timer_ = nullptr;
    mutable std::mutex wake_word_mutex_;
    std::string wake_word_;
    std::string last_emotion_;
    std::string last_chat_role_;
    std::string last_chat_text_;
    std::string last_text_;
    std::string wifi_icon_;
    bool tf_card_present_ = false;
    bool alarm_active_ = false;
    int battery_level_ = 0;
    bool battery_charging_ = false;
    std::string status_text_;
    MusicTrack music_track_;
    bool standby_ = false;
    std::string time_text_;
    DuplexMode duplex_mode_ = DuplexMode::kHalfDuplex;
    bool duplex_visible_ = false;
#ifdef CONFIG_LSPLATFORM
    std::shared_ptr<LvglImage> image_;
#endif
};
