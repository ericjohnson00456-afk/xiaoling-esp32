#include "qrcode_presenter.h"

#include "board.h"
#include "display.h"
#include "ui/models/qrcode/qrcode_model.h"
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
    presenter->last_rendered_message_.clear();
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
    if (view_ == nullptr) {
        return;
    }

#ifdef CONFIG_LSPLATFORM
    std::shared_ptr<LvglImage> image;
    std::string message;
    if (state.content_source == QrCodeContentSource::kCustom && state.custom_image != nullptr) {
        image = state.custom_image;
        message = state.custom_message;
    } else if (state.role_config_image != nullptr) {
        image = state.role_config_image;
        message = "角色配置";
    }

    if (image != nullptr) {
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
    (void)state;
#endif
}
