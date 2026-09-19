#include "qrcode_presenter.h"

#include "board.h"
#include "display/display.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"
#include "ui/ui_app.h"
#include "ui/i18n/lang_config.h"
#include "ui/utils/image/lvgl_image.h"
#include "system/device_status.h"
#include "qrcode_view.h"

#include <utility>
#include <new>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "QrCodePresenter"

namespace {
constexpr uint64_t kQrCodeTimeoutUs = 30ULL * 1000 * 1000;

struct FetchRequest {
    QrCodePresenter* presenter;
    std::string url;
    uint32_t generation;
};

class SharedLvglImage final : public LvglImage {
public:
    explicit SharedLvglImage(std::shared_ptr<LvglImage> image) : image_(std::move(image)) {
    }

    const lv_img_dsc_t* image_dsc() const override {
        return image_ != nullptr ? image_->image_dsc() : nullptr;
    }

private:
    std::shared_ptr<LvglImage> image_;
};
}

QrCodePresenter& QrCodePresenter::GetInstance() {
    static QrCodePresenter instance;
    return instance;
}

const UiNavScreen& QrCodePresenter::Screen() {
    static const UiNavScreen screen = {
        .unique_id = UI_PAGE_QRCODE,
        .open = &QrCodePresenter::Open,
        .show = &QrCodePresenter::Show,
        .pause = &QrCodePresenter::Pause,
        .resume = &QrCodePresenter::Resume,
        .close = &QrCodePresenter::Close,
    };
    return screen;
}

int QrCodePresenter::Open(const UiNavScreen* screen, void** data) {
    (void)screen;
    auto& presenter = GetInstance();
    presenter.view_ = std::make_unique<QrCodeView>(UiManager::GetInstance().display());
    *data = &presenter;
    return 0;
}

int QrCodePresenter::Show(const UiNavScreen* screen, void* data) {
    (void)screen;
    auto* presenter = static_cast<QrCodePresenter*>(data);
    if (presenter == nullptr || presenter->view_ == nullptr) {
        return -1;
    }
    presenter->view_->Show();
    // The provisioning QR must remain available until the user completes
    // BluFi setup. Other QR-code screens keep their normal auto-dismissal.
    if (DeviceStatus::GetState() == kDeviceStateWifiConfiguring) {
        presenter->DestroyTimeoutTimer();
    } else {
        presenter->StartTimeoutTimer();
    }
    presenter->Refresh();
    return 0;
}

int QrCodePresenter::Pause(const UiNavScreen* screen, void* data) {
    (void)screen;
    auto* presenter = static_cast<QrCodePresenter*>(data);
    if (presenter != nullptr) {
        presenter->DestroyTimeoutTimer();
        if (presenter->view_ != nullptr) {
            presenter->view_->Hide();
        }
        presenter->rendered_ = false;
    }
    return 0;
}

int QrCodePresenter::Resume(const UiNavScreen* screen, void* data) {
    return Show(screen, data);
}

int QrCodePresenter::Close(const UiNavScreen* screen, void* data) {
    (void)screen;
    auto* presenter = static_cast<QrCodePresenter*>(data);
    if (presenter == nullptr) {
        return 0;
    }
    presenter->DestroyTimeoutTimer();
    presenter->view_.reset();
    presenter->last_rendered_message_.clear();
    presenter->last_rendered_image_.reset();
    presenter->rendered_ = false;
    return 0;
}

void QrCodePresenter::HandleSettingMessage(const cJSON* data) {
    const auto* guide = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "mp_guide") : nullptr;
    const auto* role_config = cJSON_IsObject(guide) ? cJSON_GetObjectItemCaseSensitive(guide, "role_config") : nullptr;
    const auto* image_url = cJSON_IsObject(role_config) ? cJSON_GetObjectItemCaseSensitive(role_config, "image_url") : nullptr;
    if (cJSON_IsString(image_url) && image_url->valuestring != nullptr && image_url->valuestring[0] != '\0') {
        std::string url(image_url->valuestring);
        UiManager::GetInstance().Dispatch([url = std::move(url)]() mutable {
            QrCodePresenter::GetInstance().SetRoleConfigUrl(std::move(url));
        });
    }
}

void QrCodePresenter::SetRoleConfigUrl(std::string url) {
    if (url.empty()) {
        return;
    }

    uint32_t generation;
    std::string request_url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (role_config_url_ == url && role_config_image_ != nullptr) {
            return;
        }
        generation = ++fetch_generation_;
        role_config_url_ = std::move(url);
        role_config_image_.reset();
        request_url = role_config_url_;
    }

#if CONFIG_LSPLATFORM
    auto* request = new (std::nothrow) FetchRequest{this, request_url, generation};
    if (request == nullptr || xTaskCreate(&QrCodePresenter::FetchTask, "qrcode_fetch", 6144, request, 2, nullptr) != pdPASS) {
        delete request;
        CompleteFetch(request_url, generation, nullptr);
    }
#else
    CompleteFetch(request_url, generation, nullptr);
#endif
}

void QrCodePresenter::ClearRoleConfigUrl() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++fetch_generation_;
    role_config_url_.clear();
    role_config_image_.reset();
}

void QrCodePresenter::SelectRoleConfig() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        custom_selected_ = false;
        custom_image_.reset();
        custom_message_.clear();
    }
    UiManager::GetInstance().Dispatch([]() {
        auto& presenter = QrCodePresenter::GetInstance();
        if (presenter.view_ != nullptr && UiManager::GetInstance().TopScreenId() == UI_PAGE_QRCODE) {
            presenter.Refresh();
        }
    });
}

void QrCodePresenter::SetCustomQrCode(std::unique_ptr<LvglImage> image, std::string message) {
    if (image == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        custom_image_ = std::shared_ptr<LvglImage>(std::move(image));
        custom_message_ = std::move(message);
        custom_selected_ = true;
    }
    UiManager::GetInstance().Dispatch([]() {
        auto& presenter = QrCodePresenter::GetInstance();
        if (presenter.view_ != nullptr && UiManager::GetInstance().TopScreenId() == UI_PAGE_QRCODE) {
            presenter.Refresh();
        }
    });
}

bool QrCodePresenter::HasRoleConfigQrCode() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return role_config_image_ != nullptr;
}

void QrCodePresenter::FetchTask(void* arg) {
#if CONFIG_LSPLATFORM
    std::unique_ptr<FetchRequest> request(static_cast<FetchRequest*>(arg));
    std::unique_ptr<LvglImage> image;
    if (auto* fetcher = Board::GetInstance().GetImageFetcher(); fetcher != nullptr) {
        image = fetcher->Fetch(request->url);
    }
    request->presenter->CompleteFetch(request->url, request->generation, std::move(image));
#else
    delete static_cast<FetchRequest*>(arg);
#endif
    vTaskDelete(nullptr);
}

void QrCodePresenter::CompleteFetch(const std::string& url, uint32_t generation,
                                    std::unique_ptr<LvglImage> image) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (generation != fetch_generation_ || url != role_config_url_) {
            return;
        }
        role_config_image_ = image != nullptr ? std::shared_ptr<LvglImage>(std::move(image)) : nullptr;
    }
    UiManager::GetInstance().Dispatch([]() {
        auto& presenter = QrCodePresenter::GetInstance();
        if (presenter.view_ != nullptr && UiManager::GetInstance().TopScreenId() == UI_PAGE_QRCODE) {
            presenter.Refresh();
        }
    });
}

void QrCodePresenter::StartTimeoutTimer() {
    DestroyTimeoutTimer();

    const esp_timer_create_args_t args = {
        .callback = &QrCodePresenter::TimeoutCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "qrcode_timeout",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &timeout_timer_);
    if (err != ESP_OK) {
        timeout_timer_ = nullptr;
        ESP_LOGE(TAG, "Failed to create QR code timeout timer: %s", esp_err_to_name(err));
        return;
    }

    err = esp_timer_start_once(timeout_timer_, kQrCodeTimeoutUs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start QR code timeout timer: %s", esp_err_to_name(err));
        DestroyTimeoutTimer();
    }
}

void QrCodePresenter::DestroyTimeoutTimer() {
    if (timeout_timer_ == nullptr) {
        return;
    }

    auto timer = timeout_timer_;
    timeout_timer_ = nullptr;
    esp_timer_stop(timer);
    const esp_err_t err = esp_timer_delete(timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to delete QR code timeout timer: %s", esp_err_to_name(err));
    }
}

void QrCodePresenter::TimeoutCallback(void* arg) {
    (void)arg;
    UiApp::NavigateHome();
}

void QrCodePresenter::Refresh() {
    if (view_ == nullptr) {
        return;
    }

#ifdef CONFIG_LSPLATFORM
    std::shared_ptr<LvglImage> image;
    std::string message;
    bool custom_selected = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        custom_selected = custom_selected_;
        if (custom_selected && custom_image_ != nullptr) {
            image = custom_image_;
            message = custom_message_;
        } else if (role_config_image_ != nullptr) {
            image = role_config_image_;
        }
    }
    if (image != nullptr) {
        if (message.empty() && !custom_selected) {
            message = Lang::Strings::ROLE_CONFIGURATION;
        }
        if (rendered_ && image == last_rendered_image_ && message == last_rendered_message_) {
            return;
        }
        last_rendered_image_ = image;
        last_rendered_message_ = message;
        rendered_ = true;
        view_->ShowQrCode(std::make_unique<SharedLvglImage>(std::move(image)), message);
        return;
    }

    std::string provisioning_message;
    auto provisioning_image = Board::GetInstance().GetProvisioningQRCode(provisioning_message);
    if (provisioning_image != nullptr) {
        last_rendered_image_.reset();
        last_rendered_message_ = provisioning_message;
        rendered_ = true;
        view_->ShowQrCode(std::move(provisioning_image), provisioning_message);
        return;
    }

    // Hotspot/acoustic provisioning does not have a static image. Let the
    // board start its normal provisioning UI in that case.
    Board::GetInstance().EnterNetworkConfigMode();
#else
#endif
}
