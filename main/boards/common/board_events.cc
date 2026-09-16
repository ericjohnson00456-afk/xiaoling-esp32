#include "board_events.h"

#include <algorithm>
#include <cstring>

#include <freertos/FreeRTOS.h>

ESP_EVENT_DEFINE_BASE(BOARD_EVENT);

namespace BoardEvents {

esp_err_t PostInput(Input input, int32_t value, bool percent) {
    const InputEvent event{.input = input, .value = value, .percent = percent};
    return esp_event_post(BOARD_EVENT, kInput, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostWifiConfigHint(std::string_view ssid, std::string_view url) {
    WifiConfigHintEvent event{.ssid = {}, .url = {}};
    const size_t ssid_length = std::min(ssid.size(), sizeof(event.ssid) - 1);
    const size_t url_length = std::min(url.size(), sizeof(event.url) - 1);
    std::memcpy(event.ssid, ssid.data(), ssid_length);
    std::memcpy(event.url, url.data(), url_length);
    event.ssid[ssid_length] = '\0';
    event.url[url_length] = '\0';
    return esp_event_post(BOARD_EVENT, kWifiConfigHint, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostWakeWord(std::string_view wake_word) {
    WakeWordEvent event{.wake_word = {}};
    const size_t length = std::min(wake_word.size(), sizeof(event.wake_word) - 1);
    std::memcpy(event.wake_word, wake_word.data(), length);
    event.wake_word[length] = '\0';
    return esp_event_post(BOARD_EVENT, kWakeWordDetected, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostSystemMessage(std::string_view role, std::string_view content) {
    SystemMessageEvent event{.role = {}, .content = {}};
    const size_t role_length = std::min(role.size(), sizeof(event.role) - 1);
    const size_t content_length = std::min(content.size(), sizeof(event.content) - 1);
    std::memcpy(event.role, role.data(), role_length);
    std::memcpy(event.content, content.data(), content_length);
    event.role[role_length] = '\0';
    event.content[content_length] = '\0';
    return esp_event_post(BOARD_EVENT, kSystemMessage, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostEmotion(std::string_view emotion) {
    EmotionEvent event{.emotion = {}};
    const size_t length = std::min(emotion.size(), sizeof(event.emotion) - 1);
    std::memcpy(event.emotion, emotion.data(), length);
    event.emotion[length] = '\0';
    return esp_event_post(BOARD_EVENT, kEmotion, &event, sizeof(event), portMAX_DELAY);
}

esp_err_t PostLocalizedText(LocalizedText key) {
    const LocalizedTextEvent event{.key = key};
    return esp_event_post(BOARD_EVENT, kLocalizedText, &event, sizeof(event), portMAX_DELAY);
}

DeviceState GetRuntimeState() {
    return DeviceStatus::GetState();
}

}  // namespace BoardEvents
