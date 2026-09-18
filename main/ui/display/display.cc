#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <font_awesome.h>

#include "display.h"
#include "board.h"
#include "audio_codec.h"
#include "settings.h"
#include "ui/i18n/lang_config.h"

#define TAG "Display"

Display::Display() {
}

Display::~Display() {
}

void Display::SetStatus(const char* status) {
    ESP_LOGW(TAG, "SetStatus: %s", status);
}

void Display::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGW(TAG, "ShowNotification: %s", notification);
}

void Display::UpdateStatusBar(bool update_all) {
}


void Display::SetEmotion(const char* emotion) {
    ESP_LOGW(TAG, "SetEmotion: %s", emotion);
}

void Display::ShowEmotionOnce(const char* emotion) {
    (void)emotion;
}

void Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}

void Display::SetNetworkIcon(const char* icon) {
    (void)icon;
}

void Display::SetDuplexMode(DuplexMode mode) {
    (void)mode;
}

void Display::SetClockText(const char* text) {
    (void)text;
}

void Display::SetHomeVisible(bool visible) {
    (void)visible;
}

void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
    Settings settings("display", true);
    settings.SetString("theme", theme->name());
}

void Display::SetPowerSaveMode(bool on) {
    ESP_LOGW(TAG, "SetPowerSaveMode: %d", on);
}

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
