#pragma once

#include "ui/core/nav_stack.h"

#include <memory>
#include <mutex>
#include <string>
#include <cJSON.h>

#include <esp_timer.h>

class QrCodeView;
class LvglImage;

class QrCodePresenter {
public:
    static QrCodePresenter& GetInstance();
    static const UiNavScreen& Screen();

    // External modules push QR-code content into the page presenter. The
    // presenter owns the asynchronous role-config cache and page state.
    void HandleSettingMessage(const cJSON* data);
    void ClearRoleConfigUrl();
    void SelectRoleConfig();
    void SetCustomQrCode(std::unique_ptr<LvglImage> image, std::string message);
    bool HasRoleConfigQrCode() const;

private:
    QrCodePresenter() = default;

    static int Open(const UiNavScreen* screen, void** data);
    static int Show(const UiNavScreen* screen, void* data);
    static int Pause(const UiNavScreen* screen, void* data);
    static int Resume(const UiNavScreen* screen, void* data);
    static int Close(const UiNavScreen* screen, void* data);

    void Refresh();
    void SetRoleConfigUrl(std::string url);
    static void FetchTask(void* arg);
    void CompleteFetch(const std::string& url, uint32_t generation,
                       std::unique_ptr<LvglImage> image);
    void StartTimeoutTimer();
    void DestroyTimeoutTimer();
    static void TimeoutCallback(void* arg);

    std::unique_ptr<QrCodeView> view_;
    esp_timer_handle_t timeout_timer_ = nullptr;
    std::string last_rendered_message_;
    std::shared_ptr<LvglImage> last_rendered_image_;
    bool rendered_ = false;

    mutable std::mutex state_mutex_;
    std::string role_config_url_;
    std::shared_ptr<LvglImage> role_config_image_;
    std::shared_ptr<LvglImage> custom_image_;
    std::string custom_message_;
    bool custom_selected_ = false;
    uint32_t fetch_generation_ = 0;
};
