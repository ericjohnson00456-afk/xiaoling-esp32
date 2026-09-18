#pragma once

#include "ui/core/nav_stack.h"
#include "xiaoling/session/session_core.h"

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

    void SetBannerIdleProvider(std::function<bool()> provider);
    void SetWakeWordProvider(std::function<std::string()> provider);
    void HandleSettingMessage(const cJSON* data);
    void SetEmotion(std::string emotion);
    void ShowEmotionOnce(std::string emotion);
    void SetPowerSaveMode(bool enabled);
    void SetText(std::string text);
    void SetChatMessage(std::string role, std::string text);
    void ShowNotification(std::string text, int duration_ms = 3000);
    void SetWifiIcon(std::string icon);
    void SetStatus(std::string status);
    void SetTime(std::string time);
    void SetDuplexMode(DuplexMode mode);
#ifdef CONFIG_LSPLATFORM
    void SetImage(std::unique_ptr<LvglImage> image);
    void ClearImage();
#endif

private:
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
    void StopBannerTimer();
    void OnBannerTick();
    static void BannerTimerCallback(void* arg);

    std::unique_ptr<HomeView> view_;
    std::mutex banner_mutex_;
    std::vector<std::string> banner_texts_;
    size_t banner_index_ = 0;
    esp_timer_handle_t banner_timer_ = nullptr;
    std::function<bool()> banner_idle_provider_;
    std::vector<uint32_t> voice_event_subscriptions_;
    mutable std::mutex wake_word_mutex_;
    std::function<std::string()> wake_word_provider_;
    std::string last_emotion_;
    std::string last_chat_role_;
    std::string last_chat_text_;
    uint32_t last_voice_text_revision_ = 0;
    std::string last_text_;
    std::string wifi_icon_;
    std::string status_text_;
    std::string time_text_;
    DuplexMode duplex_mode_ = DuplexMode::kHalfDuplex;
#ifdef CONFIG_LSPLATFORM
    std::shared_ptr<LvglImage> image_;
#endif
};
