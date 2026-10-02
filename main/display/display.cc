#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <font_awesome.h>

#include "display/display.h"
#include "board.h"
#include "codecs/audio_codec.h"
#include "settings.h"
#include "ui/i18n/lang_config.h"

#define TAG "Display"

Display::Display() {
}

Display::~Display() {
}

void Display::SetStatus(const char* status) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleStatus(status)) return;
    ESP_LOGW(TAG, "SetStatus: %s", status);
}

void Display::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleNotification(notification, duration_ms)) return;
    ESP_LOGW(TAG, "ShowNotification: %s", notification);
}

void Display::UpdateStatusBar(bool update_all) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleStatusBarUpdate(update_all)) return;
}


void Display::SetEmotion(const char* emotion) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleEmotion(emotion, false)) return;
    ESP_LOGW(TAG, "SetEmotion: %s", emotion);
}

void Display::ShowEmotionOnce(const char* emotion) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleEmotion(emotion, true)) return;
    (void)emotion;
}

void Display::SetChatMessage(const char* role, const char* content) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleChatMessage(role, content)) return;
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}

void Display::SetNetworkIcon(const char* icon) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleNetworkIcon(icon)) return;
    (void)icon;
}

void Display::SetDuplexMode(DuplexMode mode) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleDuplexMode(mode)) return;
    (void)mode;
}

void Display::SetClockText(const char* text) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleClockText(text)) return;
    (void)text;
}

void Display::SetHomeVisible(bool visible) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleHomeVisible(visible)) return;
    (void)visible;
}

void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
    Settings settings("display", true);
    settings.SetString("theme", theme->name());
}

void Display::SetPowerSaveMode(bool on) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandlePowerSaveMode(on)) return;
    ESP_LOGW(TAG, "SetPowerSaveMode: %d", on);
}

#ifdef CONFIG_LSPLATFORM
void Display::ShowActivation(std::unique_ptr<LvglImage> qrcode, const std::string& message) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleActivation(std::move(qrcode), message)) return;
}

void Display::DismissActivation() {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleDismissActivation()) return;
}

void Display::ShowImage(std::unique_ptr<LvglImage> image) {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleImage(std::move(image))) return;
}

void Display::DismissImage() {
    if (ui_adapter_ != nullptr && ui_adapter_->HandleDismissImage()) return;
}
#endif // CONFIG_LSPLATFORM

void Display::SetBrightness(uint8_t brightness) {
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightness(brightness, true);
    }
}

uint8_t Display::GetBrightness() {
    auto backlight = Board::GetInstance().GetBacklight();
    return backlight != nullptr ? backlight->brightness() : 0;
}
