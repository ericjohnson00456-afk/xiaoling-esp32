#pragma once

#include "display/display.h"

// Bridges the legacy Display semantic API to the current presenter/view UI.
// It owns no LVGL objects; all rendering remains in the active View.
class LegacyDisplayAdapter final : public DisplayUiAdapter {
public:
    bool HandleStatus(const char* status) override;
    bool HandleNotification(const char* notification, int duration_ms) override;
    bool HandleEmotion(const char* emotion, bool once) override;
    bool HandleChatMessage(const char* role, const char* content) override;
    bool HandleNetworkIcon(const char* icon) override;
    bool HandleDuplexMode(DuplexMode mode) override;
    bool HandleClockText(const char* text) override;
    bool HandleHomeVisible(bool visible) override;
    bool HandleStatusBarUpdate(bool update_all) override;
    bool HandlePowerSaveMode(bool on) override;

#ifdef CONFIG_LSPLATFORM
    bool HandlePreviewImage(std::unique_ptr<LvglImage> image) override;
    bool HandleActivation(std::unique_ptr<LvglImage> qrcode,
                          const std::string& message) override;
    bool HandleDismissActivation() override;
    bool HandleImage(std::unique_ptr<LvglImage> image) override;
    bool HandleDismissImage() override;
#endif
};
