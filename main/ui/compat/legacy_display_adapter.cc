#include "legacy_display_adapter.h"

#include "ui/ui.h"
#include "ui/utils/image/lvgl_image.h"

#include <utility>

namespace {

std::string CopyText(const char* text) {
    return text != nullptr ? text : "";
}

}  // namespace

bool LegacyDisplayAdapter::HandleStatus(const char* status) {
    Ui::SetStatusText(CopyText(status));
    return true;
}

bool LegacyDisplayAdapter::HandleNotification(const char* notification, int duration_ms) {
    Ui::ShowNotification(CopyText(notification), duration_ms);
    return true;
}

bool LegacyDisplayAdapter::HandleEmotion(const char* emotion, bool once) {
    auto value = CopyText(emotion);
    if (once) {
        Ui::ShowEmotionOnce(std::move(value));
    } else {
        Ui::SetEmotion(std::move(value));
    }
    return true;
}

bool LegacyDisplayAdapter::HandleChatMessage(const char* role, const char* content) {
    Ui::SetChatMessage(CopyText(role), CopyText(content));
    return true;
}

bool LegacyDisplayAdapter::HandleNetworkIcon(const char* icon) {
    Ui::SetWifiIcon(CopyText(icon));
    return true;
}

bool LegacyDisplayAdapter::HandleDuplexMode(DuplexMode mode) {
    Ui::SetDuplexMode(mode);
    return true;
}

bool LegacyDisplayAdapter::HandleClockText(const char* text) {
    Ui::SetClockText(CopyText(text));
    return true;
}

bool LegacyDisplayAdapter::HandleHomeVisible(bool visible) {
    if (!visible) {
        return true;
    }
    Ui::NavigateHome();
    return true;
}

bool LegacyDisplayAdapter::HandleStatusBarUpdate(bool update_all) {
    Ui::UpdateStatusBar(update_all);
    return true;
}

bool LegacyDisplayAdapter::HandlePowerSaveMode(bool on) {
    Ui::SetPowerSaveMode(on);
    return true;
}

#ifdef CONFIG_LSPLATFORM
bool LegacyDisplayAdapter::HandlePreviewImage(std::unique_ptr<LvglImage> image) {
    if (image != nullptr) {
        Ui::ShowImage(std::move(image));
    } else {
        Ui::DismissImage();
    }
    return true;
}

bool LegacyDisplayAdapter::HandleActivation(std::unique_ptr<LvglImage> qrcode,
                                            const std::string& message) {
    if (qrcode == nullptr) {
        return true;
    }
    Ui::ShowQrCode(std::move(qrcode), message);
    return true;
}

bool LegacyDisplayAdapter::HandleDismissActivation() {
    Ui::DismissQrCode();
    return true;
}

bool LegacyDisplayAdapter::HandleImage(std::unique_ptr<LvglImage> image) {
    return HandlePreviewImage(std::move(image));
}

bool LegacyDisplayAdapter::HandleDismissImage() {
    Ui::DismissImage();
    return true;
}
#endif // CONFIG_LSPLATFORM
