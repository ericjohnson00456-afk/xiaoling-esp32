#include "ui_events.h"

#include <algorithm>
#include <cstring>

#include <freertos/FreeRTOS.h>

ESP_EVENT_DEFINE_BASE(UI_EVENT);

namespace UiEvents {

esp_err_t PostStatus(Status status, int32_t value, std::string_view detail) {
    StatusEvent event{
        .status = status,
        .value = value,
        .detail = {},
    };
    const size_t length = std::min(detail.size(), sizeof(event.detail) - 1);
    if (length > 0) {
        std::memcpy(event.detail, detail.data(), length);
        event.detail[length] = '\0';
    }
    return esp_event_post(UI_EVENT, kStatusChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostStatusText(std::string_view text) {
    StatusTextEvent event{.text = {}};
    const size_t length = std::min(text.size(), sizeof(event.text) - 1);
    if (length > 0) {
        std::memcpy(event.text, text.data(), length);
        event.text[length] = '\0';
    }
    return esp_event_post(UI_EVENT, kStatusTextChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostNotification(Notification notification, int32_t duration_ms, std::string_view detail) {
    NotificationEvent event{
        .notification = notification,
        .duration_ms = duration_ms,
        .detail = {},
    };
    const size_t length = std::min(detail.size(), sizeof(event.detail) - 1);
    if (length > 0) {
        std::memcpy(event.detail, detail.data(), length);
        event.detail[length] = '\0';
    }
    return esp_event_post(UI_EVENT, kNotificationChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostChatMessage(std::string_view role, std::string_view content) {
    ChatMessageEvent event{.role = {}, .content = {}};
    const size_t role_length = std::min(role.size(), sizeof(event.role) - 1);
    const size_t content_length = std::min(content.size(), sizeof(event.content) - 1);
    std::memcpy(event.role, role.data(), role_length);
    std::memcpy(event.content, content.data(), content_length);
    event.role[role_length] = '\0';
    event.content[content_length] = '\0';
    return esp_event_post(UI_EVENT, kChatMessageChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostVoicePrompt(std::string_view text) {
    VoicePromptEvent event{.text = {}};
    const size_t length = std::min(text.size(), sizeof(event.text) - 1);
    std::memcpy(event.text, text.data(), length);
    event.text[length] = '\0';
    return esp_event_post(UI_EVENT, kVoicePromptChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostDuplexMode(int32_t mode) {
    DuplexModeEvent event{.mode = mode};
    return esp_event_post(UI_EVENT, kDuplexModeChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostQrCodeRequested() {
    return esp_event_post(UI_EVENT, kQrCodeRequested, nullptr, 0, portMAX_DELAY);
}

esp_err_t PostHomeRequested() {
    return esp_event_post(UI_EVENT, kHomeRequested, nullptr, 0, portMAX_DELAY);
}

esp_err_t PostEmotion(std::string_view emotion) {
    EmotionEvent event{.emotion = {}};
    const size_t length = std::min(emotion.size(), sizeof(event.emotion) - 1);
    std::memcpy(event.emotion, emotion.data(), length);
    event.emotion[length] = '\0';
    return esp_event_post(UI_EVENT, kEmotionChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostStatusBarUpdate(bool update_all) {
    StatusBarEvent event{.update_all = update_all};
    return esp_event_post(UI_EVENT, kStatusBarChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostNotificationText(std::string_view text, int32_t duration_ms) {
    NotificationTextEvent event{.duration_ms = duration_ms, .text = {}};
    const size_t length = std::min(text.size(), sizeof(event.text) - 1);
    std::memcpy(event.text, text.data(), length);
    event.text[length] = '\0';
    return esp_event_post(UI_EVENT, kNotificationTextChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostDismissOverlays() {
    return esp_event_post(UI_EVENT, kOverlaysDismissed, nullptr, 0, portMAX_DELAY);
}

esp_err_t PostVolume(int32_t volume, bool percent) {
    VolumeEvent event{.action = 0, .volume = volume, .percent = percent};
    return esp_event_post(UI_EVENT, kVolumeChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostVolumeMax() {
    VolumeEvent event{.action = 1, .volume = 100, .percent = false};
    return esp_event_post(UI_EVENT, kVolumeChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostVolumeMuted() {
    VolumeEvent event{.action = 2, .volume = 0, .percent = false};
    return esp_event_post(UI_EVENT, kVolumeChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostLocalizedText(TextKey key) {
    LocalizedTextEvent event{.key = key};
    return esp_event_post(UI_EVENT, kLocalizedTextChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostWifiConfigHint(std::string_view ssid, std::string_view url) {
    WifiConfigHintEvent event{.ssid = {}, .url = {}};
    const size_t ssid_length = std::min(ssid.size(), sizeof(event.ssid) - 1);
    const size_t url_length = std::min(url.size(), sizeof(event.url) - 1);
    std::memcpy(event.ssid, ssid.data(), ssid_length);
    std::memcpy(event.url, url.data(), url_length);
    event.ssid[ssid_length] = '\0';
    event.url[url_length] = '\0';
    return esp_event_post(UI_EVENT, kWifiConfigHintChanged, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostPowerSaveMode(bool enabled) {
    PowerSaveModeEvent event{.enabled = enabled};
    return esp_event_post(UI_EVENT, kPowerSaveModeChanged, &event, sizeof(event), portMAX_DELAY);
}

}  // namespace UiEvents
