#include "legacy_display_adapter.h"

#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"
#include "ui/events/ui_events.h"
#include "ui/screens/home/home_presenter.h"
#include "ui/screens/qrcode/qrcode_presenter.h"
#include "ui/utils/image/lvgl_image.h"

#include <utility>

namespace {

std::string CopyText(const char* text) {
    return text != nullptr ? text : "";
}

}  // namespace

bool LegacyDisplayAdapter::HandleStatus(const char* status) {
    auto value = CopyText(status);
    UiManager::GetInstance().Dispatch([value = std::move(value)]() mutable {
        HomePresenter::GetInstance().SetStatus(std::move(value));
    });
    return true;
}

bool LegacyDisplayAdapter::HandleNotification(const char* notification, int duration_ms) {
    auto value = CopyText(notification);
    UiManager::GetInstance().Dispatch([value = std::move(value), duration_ms]() mutable {
        HomePresenter::GetInstance().ShowNotification(std::move(value), duration_ms);
    });
    return true;
}

bool LegacyDisplayAdapter::HandleEmotion(const char* emotion, bool once) {
    auto value = CopyText(emotion);
    UiManager::GetInstance().Dispatch([value = std::move(value), once]() mutable {
        if (once) {
            HomePresenter::GetInstance().ShowEmotionOnce(std::move(value));
        } else {
            HomePresenter::GetInstance().SetEmotion(std::move(value));
        }
    });
    return true;
}

bool LegacyDisplayAdapter::HandleChatMessage(const char* role, const char* content) {
    auto role_value = CopyText(role);
    auto content_value = CopyText(content);
    UiManager::GetInstance().Dispatch([role = std::move(role_value),
                                       content = std::move(content_value)]() mutable {
        HomePresenter::GetInstance().SetChatMessage(std::move(role), std::move(content));
    });
    return true;
}

bool LegacyDisplayAdapter::HandleNetworkIcon(const char* icon) {
    auto value = CopyText(icon);
    UiManager::GetInstance().Dispatch([value = std::move(value)]() mutable {
        HomePresenter::GetInstance().SetWifiIcon(std::move(value));
    });
    return true;
}

bool LegacyDisplayAdapter::HandleDuplexMode(DuplexMode mode) {
    UiManager::GetInstance().Dispatch([mode]() {
        HomePresenter::GetInstance().SetDuplexMode(mode);
    });
    return true;
}

bool LegacyDisplayAdapter::HandleClockText(const char* text) {
    auto value = CopyText(text);
    UiManager::GetInstance().Dispatch([value = std::move(value)]() mutable {
        HomePresenter::GetInstance().SetTime(std::move(value));
    });
    return true;
}

bool LegacyDisplayAdapter::HandleHomeVisible(bool visible) {
    if (!visible) {
        return true;
    }
    UiManager::GetInstance().Dispatch([]() {
        UiManager::GetInstance().NavigateHome();
    });
    return true;
}

bool LegacyDisplayAdapter::HandleStatusBarUpdate(bool update_all) {
    // Status-bar values are now pushed to the active View by their presenters.
    // Keep the legacy call as a handled no-op to avoid routing it back through
    // UiEventHandler and recursively calling Display::UpdateStatusBar().
    (void)update_all;
    return true;
}

bool LegacyDisplayAdapter::HandlePowerSaveMode(bool on) {
    UiManager::GetInstance().Dispatch([on]() {
        HomePresenter::GetInstance().SetPowerSaveMode(on);
    });
    return true;
}

#ifdef CONFIG_LSPLATFORM
bool LegacyDisplayAdapter::HandlePreviewImage(std::unique_ptr<LvglImage> image) {
    if (image != nullptr) {
        HomePresenter::GetInstance().SetImage(std::move(image));
    } else {
        HomePresenter::GetInstance().ClearImage();
    }
    return true;
}

bool LegacyDisplayAdapter::HandleActivation(std::unique_ptr<LvglImage> qrcode,
                                            const std::string& message) {
    if (qrcode == nullptr) {
        return true;
    }
    QrCodePresenter::GetInstance().SetCustomQrCode(std::move(qrcode), message);
    UiManager::GetInstance().Dispatch([]() {
        UiManager::GetInstance().NavigateTo(UI_PAGE_QRCODE);
    });
    return true;
}

bool LegacyDisplayAdapter::HandleDismissActivation() {
    UiManager::GetInstance().Dispatch([]() {
        if (UiManager::GetInstance().TopScreenId() == UI_PAGE_QRCODE) {
            UiManager::GetInstance().NavigateHome();
        }
    });
    return true;
}

bool LegacyDisplayAdapter::HandleImage(std::unique_ptr<LvglImage> image) {
    return HandlePreviewImage(std::move(image));
}

bool LegacyDisplayAdapter::HandleDismissImage() {
    HomePresenter::GetInstance().ClearImage();
    return true;
}
#endif // CONFIG_LSPLATFORM
