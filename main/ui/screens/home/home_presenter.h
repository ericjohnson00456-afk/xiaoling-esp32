#pragma once

#include "ui/core/nav_stack.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_timer.h>

class HomeView;

// Home 页 Presenter：组合语音、Banner、Wi-Fi、状态和网络图片 Model。
class HomePresenter {
public:
    static HomePresenter& GetInstance();
    static const UiNavScreen& Screen();

    void SetBannerIdleProvider(std::function<bool()> provider);

private:
    HomePresenter() = default;

    static int Open(const UiNavScreen* screen, void** data);
    static int Show(const UiNavScreen* screen, void* data);
    static int Pause(const UiNavScreen* screen, void* data);
    static int Resume(const UiNavScreen* screen, void* data);
    static int Close(const UiNavScreen* screen, void* data);

    void SubscribeModels();
    void UnsubscribeModels();
#ifdef CONFIG_LSPLATFORM
    void RefreshImage(const struct ImageModelState& state);
#endif
    void SetBanners(std::vector<std::string> texts, uint32_t interval_ms);
    void StopBannerTimer();
    void OnBannerTick();
    static void BannerTimerCallback(void* arg);

    std::unique_ptr<HomeView> view_;
    std::mutex banner_mutex_;
    std::vector<std::string> banner_texts_;
    size_t banner_index_ = 0;
    esp_timer_handle_t banner_timer_ = nullptr;
    std::function<bool()> banner_idle_provider_;
    uint32_t voice_subscription_ = 0;
    uint32_t banner_subscription_ = 0;
    uint32_t wifi_subscription_ = 0;
    uint32_t status_subscription_ = 0;
    uint32_t duplex_subscription_ = 0;
    uint32_t image_subscription_ = 0;
    std::string last_emotion_;
    uint32_t last_voice_text_revision_ = 0;
};
