#include "qrcode_presenter.h"

#include "board.h"
#include "display.h"
#include "qrcode_model.h"
#include "ui/core/ui_manager.h"
#include "ui/core/ui_page_ids.h"
#include "ui/events/ui_events.h"
#include "ui/tools/image/lvgl_image.h"
#include "qrcode_view.h"

#include <utility>

#include <esp_log.h>

#define TAG "QrCodePresenter"

namespace {
constexpr uint64_t kQrCodeTimeoutUs = 30ULL * 1000 * 1000;

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
    presenter.SubscribeModel();
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
    presenter->StartTimeoutTimer();
    presenter->Refresh(QrCodeModel::GetInstance().Snapshot());
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
    presenter->UnsubscribeModel();
    presenter->view_.reset();
    presenter->last_rendered_url_.clear();
    presenter->last_rendered_image_.reset();
    presenter->rendered_ = false;
    return 0;
}

void QrCodePresenter::SubscribeModel() {
    qrcode_subscription_ = QrCodeModel::GetInstance().Subscribe([](const QrCodeModelState& state) {
        UiManager::GetInstance().Dispatch([state]() {
            auto& presenter = QrCodePresenter::GetInstance();
            if (presenter.view_ != nullptr && UiManager::GetInstance().TopScreenId() == UI_PAGE_QRCODE) {
                presenter.Refresh(state);
            }
        });
    }, false);
}

void QrCodePresenter::UnsubscribeModel() {
    QrCodeModel::GetInstance().Unsubscribe(qrcode_subscription_);
    qrcode_subscription_ = 0;
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
    const esp_err_t err = UiEvents::PostHomeRequested();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post QR code timeout event: %s", esp_err_to_name(err));
    }
}

void QrCodePresenter::Refresh(const QrCodeModelState& state) {
    if (view_ == nullptr ||
        (rendered_ && state.role_config_url == last_rendered_url_ &&
         state.role_config_image == last_rendered_image_)) {
        return;
    }
    last_rendered_url_ = state.role_config_url;
    last_rendered_image_ = state.role_config_image;
    rendered_ = true;

#ifdef CONFIG_LSPLATFORM
    if (state.role_config_image != nullptr) {
        view_->ShowQrCode(
            std::make_unique<SharedLvglImage>(state.role_config_image), "角色配置");
        return;
    }

    std::string message;
    auto image = Board::GetInstance().GetProvisioningQRCode(message);
    if (image != nullptr) {
        view_->ShowQrCode(std::move(image), message);
        return;
    }

    // Hotspot/acoustic provisioning does not have a static image. Let the
    // board start its normal provisioning UI in that case.
    Board::GetInstance().EnterNetworkConfigMode();
#else
    (void)state;
#endif
}
