#pragma once

#include "ui/core/nav_stack.h"

#include <memory>
#include <string>

#include <esp_timer.h>

class QrCodeView;
class LvglImage;
struct QrCodeModelState;

class QrCodePresenter {
public:
    static QrCodePresenter& GetInstance();
    static const UiNavScreen& Screen();

private:
    QrCodePresenter() = default;

    static int Open(const UiNavScreen* screen, void** data);
    static int Show(const UiNavScreen* screen, void* data);
    static int Pause(const UiNavScreen* screen, void* data);
    static int Resume(const UiNavScreen* screen, void* data);
    static int Close(const UiNavScreen* screen, void* data);

    void SubscribeModel();
    void UnsubscribeModel();
    void Refresh(const QrCodeModelState& state);
    void StartTimeoutTimer();
    void DestroyTimeoutTimer();
    static void TimeoutCallback(void* arg);

    std::unique_ptr<QrCodeView> view_;
    uint32_t qrcode_subscription_ = 0;
    esp_timer_handle_t timeout_timer_ = nullptr;
    std::string last_rendered_url_;
    std::shared_ptr<LvglImage> last_rendered_image_;
    bool rendered_ = false;
};
