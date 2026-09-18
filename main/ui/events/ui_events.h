#ifndef UI_EVENTS_H
#define UI_EVENTS_H

#include <cstdint>
#include <cstdio>
#include <string_view>

#include <esp_event.h>

ESP_EVENT_DECLARE_BASE(UI_EVENT);

namespace UiEvents {

enum class Status : int32_t {
    Initializing,
    ScanningWifi,
    RegisteringNetwork,
    DetectingModule,
    CheckingNewVersion,
    LoadingProtocol,
    Standby,
    Connecting,
    Listening,
    Speaking,
};

enum class Notification : int32_t {
    ScanningWifi,
    ConnectingToNetwork,
    ConnectedToNetwork,
    SwitchTo4G,
    SwitchToWifi,
    EnteringWifiConfigMode,
};

enum class TextKey : int32_t {
    Error,
    PleaseWait,
};

constexpr int32_t kStatusChanged = 0;
constexpr int32_t kStatusTextChanged = 1;
constexpr int32_t kNotificationChanged = 2;
constexpr int32_t kChatMessageChanged = 3;
constexpr int32_t kEmotionChanged = 4;
constexpr int32_t kStatusBarChanged = 5;
constexpr int32_t kNotificationTextChanged = 6;
constexpr int32_t kOverlaysDismissed = 7;
constexpr int32_t kVolumeChanged = 8;
constexpr int32_t kLocalizedTextChanged = 9;
constexpr int32_t kWifiConfigHintChanged = 10;
constexpr int32_t kPowerSaveModeChanged = 11;
constexpr int32_t kVoicePromptChanged = 12;
constexpr int32_t kDuplexModeChanged = 13;
constexpr int32_t kQrCodeRequested = 14;
constexpr int32_t kHomeRequested = 15;

struct StatusEvent {
    Status status;
    int32_t value;
    char detail[96];
};

struct StatusTextEvent {
    char text[128];
};

struct NotificationEvent {
    Notification notification;
    int32_t duration_ms;
    char detail[96];
};

struct ChatMessageEvent {
    char role[16];
    char content[256];
};

struct EmotionEvent {
    char emotion[32];
    bool once;
};

struct StatusBarEvent {
    bool update_all;
};

struct NotificationTextEvent {
    int32_t duration_ms;
    char text[256];
};

struct VolumeEvent {
    int32_t action;
    int32_t volume;
    bool percent;
};

struct LocalizedTextEvent {
    TextKey key;
};

struct WifiConfigHintEvent {
    char ssid[64];
    char url[128];
};

struct PowerSaveModeEvent {
    bool enabled;
};

struct VoicePromptEvent {
    char text[128];
};

struct DuplexModeEvent {
    int32_t mode;
};

esp_err_t PostStatus(Status status, int32_t value = 0, std::string_view detail = {});
esp_err_t PostStatusText(std::string_view text);
esp_err_t PostNotification(Notification notification, int32_t duration_ms = 3000,
                            std::string_view detail = {});
esp_err_t PostChatMessage(std::string_view role, std::string_view content);
esp_err_t PostEmotion(std::string_view emotion);
esp_err_t PostEmotionOnce(std::string_view emotion);
esp_err_t PostStatusBarUpdate(bool update_all = false);
esp_err_t PostNotificationText(std::string_view text, int32_t duration_ms = 3000);
esp_err_t PostDismissOverlays();
esp_err_t PostVolume(int32_t volume, bool percent = false);
esp_err_t PostVolumeMax();
esp_err_t PostVolumeMuted();
esp_err_t PostLocalizedText(TextKey key);
esp_err_t PostWifiConfigHint(std::string_view ssid, std::string_view url);
esp_err_t PostPowerSaveMode(bool enabled);
esp_err_t PostVoicePrompt(std::string_view text);
esp_err_t PostDuplexMode(int32_t mode);
esp_err_t PostQrCodeRequested();
esp_err_t PostHomeRequested();

}  // namespace UiEvents

#endif  // UI_EVENTS_H
