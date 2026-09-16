#ifndef BOARD_EVENTS_H
#define BOARD_EVENTS_H

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <atomic>

#include "system/device_status.h"

#include <esp_err.h>
#include <esp_event.h>

// Hardware input events are the only dependency a board implementation needs
// for forwarding buttons to the application.  The board publishes these
// events; the application decides what they mean for the current device state.
ESP_EVENT_DECLARE_BASE(BOARD_EVENT);

namespace BoardEvents {

enum class Input : int32_t {
    PrimaryClick,
    PrimaryDoubleClick,
    TouchDown,
    TouchUp,
    VolumeUp,
    VolumeUpLongPress,
    VolumeDown,
    VolumeDownLongPress,
    PowerSaveOn,
    PowerSaveOff,
    StatusBarRefresh,
    NetworkSwitchTo4G,
    NetworkSwitchToWifi,
    RebootRequested,
    WifiConfigNotification,
    EnterWifiConfigState,
    EnterIdleState,
    ResetProtocolRequested,
    AecToggle,
    AecSet,
    PlayToneRequested,
    VolumeChanged,
    VolumeMax,
    VolumeMuted,
};

struct InputEvent {
    Input input;
    int32_t value;
    bool percent;
};

constexpr int32_t kInput = 0;
constexpr int32_t kWifiConfigHint = 1;
constexpr int32_t kWakeWordDetected = 2;
constexpr int32_t kSystemMessage = 3;
constexpr int32_t kEmotion = 4;
constexpr int32_t kLocalizedText = 5;

esp_err_t PostInput(Input input, int32_t value = 0, bool percent = false);

struct WifiConfigHintEvent {
    char ssid[64];
    char url[128];
};

esp_err_t PostWifiConfigHint(std::string_view ssid, std::string_view url);

struct WakeWordEvent {
    char wake_word[64];
};

esp_err_t PostWakeWord(std::string_view wake_word);

// Hardware-originated user-facing hints.  Boards publish these as intents;
// Application translates them to UI events so the board never includes UI
// headers or calls UI code directly.
struct SystemMessageEvent {
    char role[16];
    char content[256];
};

esp_err_t PostSystemMessage(std::string_view role, std::string_view content);

struct EmotionEvent {
    char emotion[32];
};

esp_err_t PostEmotion(std::string_view emotion);

enum class LocalizedText : int32_t {
    Error,
    PleaseWait,
};

struct LocalizedTextEvent {
    LocalizedText key;
};

esp_err_t PostLocalizedText(LocalizedText key);

// Read-only runtime state projection for hardware callbacks. Boards can use
// this snapshot without depending on Application.
DeviceState GetRuntimeState();

}  // namespace BoardEvents

#endif  // BOARD_EVENTS_H
