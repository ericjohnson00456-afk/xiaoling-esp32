#include "ui_event_handler.h"
#include "ui_events.h"

#include "ui/i18n/lang_config.h"
#include "status_model.h"
#include "ui/core/ui_manager.h"

#include "display.h"
#include "voice_model.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>

#define TAG "UiEventHandler"

namespace {

std::string LocalizeStatus(const UiEvents::StatusEvent& event) {
    switch (event.status) {
        case UiEvents::Status::Initializing:
            return Lang::Strings::INITIALIZING;
        case UiEvents::Status::ScanningWifi:
            return Lang::Strings::SCANNING_WIFI;
        case UiEvents::Status::RegisteringNetwork:
            return Lang::Strings::REGISTERING_NETWORK;
        case UiEvents::Status::DetectingModule:
            return Lang::Strings::DETECTING_MODULE;
        case UiEvents::Status::CheckingNewVersion:
            return Lang::Strings::CHECKING_NEW_VERSION;
        case UiEvents::Status::LoadingProtocol:
            return Lang::Strings::LOADING_PROTOCOL;
        case UiEvents::Status::Standby:
            return Lang::Strings::STANDBY;
        case UiEvents::Status::Connecting:
            return Lang::Strings::CONNECTING;
        case UiEvents::Status::Listening:
            return Lang::Strings::LISTENING;
        case UiEvents::Status::Speaking:
            return Lang::Strings::SPEAKING;
    }
    return Lang::Strings::INITIALIZING;
}

std::string LocalizeNotification(const UiEvents::NotificationEvent& event) {
    switch (event.notification) {
        case UiEvents::Notification::ScanningWifi:
            return Lang::Strings::SCANNING_WIFI;
        case UiEvents::Notification::ConnectingToNetwork:
            return std::string(Lang::Strings::CONNECT_TO) + event.detail + "...";
        case UiEvents::Notification::ConnectedToNetwork:
            return std::string(Lang::Strings::CONNECTED_TO) + event.detail;
        case UiEvents::Notification::SwitchTo4G:
            return Lang::Strings::SWITCH_TO_4G_NETWORK;
        case UiEvents::Notification::SwitchToWifi:
            return Lang::Strings::SWITCH_TO_WIFI_NETWORK;
        case UiEvents::Notification::EnteringWifiConfigMode:
            return Lang::Strings::ENTERING_WIFI_CONFIG_MODE;
    }
    return {};
}

void OnStatusChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::StatusEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        StatusModel::GetInstance().SetText(LocalizeStatus(event));
    });
}

void OnStatusTextChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::StatusTextEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        StatusModel::GetInstance().SetText(event.text);
    });
}

void OnNotificationChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::NotificationEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        auto* display = UiManager::GetInstance().display();
        if (display == nullptr) {
            return;
        }
        const auto text = LocalizeNotification(event);
        if (!text.empty()) {
            display->ShowNotification(text, event.duration_ms);
        }
    });
}

void OnChatMessageChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::ChatMessageEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        auto* display = UiManager::GetInstance().display();
        if (display != nullptr) {
            display->SetChatMessage(event.role, event.content);
        }
    });
}

void OnEmotionChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::EmotionEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        VoiceModel::GetInstance().SetEmotion(event.emotion);
    });
}

void OnStatusBarChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::StatusBarEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        auto* display = UiManager::GetInstance().display();
        if (display != nullptr) {
            display->UpdateStatusBar(event.update_all);
        }
    });
}

void OnNotificationTextChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::NotificationTextEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        auto* display = UiManager::GetInstance().display();
        if (display != nullptr) {
            display->ShowNotification(event.text, event.duration_ms);
        }
    });
}

void OnOverlaysDismissed(void*, esp_event_base_t, int32_t, void*) {
    UiManager::GetInstance().Dispatch([]() {
        auto* display = UiManager::GetInstance().display();
        if (display == nullptr) {
            return;
        }
#ifdef CONFIG_LSPLATFORM
        display->DismissImage();
        display->DismissActivation();
#endif
    });
}

void OnVolumeChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::VolumeEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        auto* display = UiManager::GetInstance().display();
        if (display == nullptr) {
            return;
        }
        std::string text;
        if (event.action == 1) {
            text = Lang::Strings::MAX_VOLUME;
        } else if (event.action == 2) {
            text = Lang::Strings::MUTED;
        } else {
            text = std::string(Lang::Strings::VOLUME) + std::to_string(event.volume);
            if (event.percent) {
                text += "%";
            }
        }
        display->ShowNotification(text);
    });
}

void OnLocalizedTextChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::LocalizedTextEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        std::string text;
        switch (event.key) {
            case UiEvents::TextKey::Error:
                text = Lang::Strings::ERROR;
                break;
            case UiEvents::TextKey::PleaseWait:
                text = Lang::Strings::PLEASE_WAIT;
                break;
        }
        StatusModel::GetInstance().SetText(text);
    });
}

void OnWifiConfigHintChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::WifiConfigHintEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        std::string hint = std::string(Lang::Strings::CONNECT_TO_HOTSPOT) + event.ssid;
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += event.url;
        StatusModel::GetInstance().SetText(Lang::Strings::WIFI_CONFIG_MODE);
        VoiceModel::GetInstance().SetEmotion("gear");
        auto* display = UiManager::GetInstance().display();
        if (display != nullptr) {
            display->SetChatMessage("system", hint.c_str());
        }
    });
}

void OnPowerSaveModeChanged(void*, esp_event_base_t, int32_t, void* event_data) {
    const auto event = *static_cast<const UiEvents::PowerSaveModeEvent*>(event_data);
    UiManager::GetInstance().Dispatch([event]() {
        if (auto* display = UiManager::GetInstance().display(); display != nullptr) {
            display->SetPowerSaveMode(event.enabled);
        }
    });
}

}  // namespace

namespace UiEventHandler {

esp_err_t Initialize() {
    static bool initialized = false;
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t ret = esp_event_handler_register(UI_EVENT, UiEvents::kStatusChanged, &OnStatusChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register status event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_register(UI_EVENT, UiEvents::kStatusTextChanged, &OnStatusTextChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register status text event handler: %s", esp_err_to_name(ret));
        esp_event_handler_unregister(UI_EVENT, UiEvents::kStatusChanged, &OnStatusChanged);
        return ret;
    }

    ret = esp_event_handler_register(UI_EVENT, UiEvents::kNotificationChanged, &OnNotificationChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register notification event handler: %s", esp_err_to_name(ret));
        esp_event_handler_unregister(UI_EVENT, UiEvents::kStatusChanged, &OnStatusChanged);
        esp_event_handler_unregister(UI_EVENT, UiEvents::kStatusTextChanged, &OnStatusTextChanged);
        return ret;
    }

    ret = esp_event_handler_register(UI_EVENT, UiEvents::kChatMessageChanged, &OnChatMessageChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register chat message event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_event_handler_register(UI_EVENT, UiEvents::kEmotionChanged, &OnEmotionChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register emotion event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_event_handler_register(UI_EVENT, UiEvents::kStatusBarChanged, &OnStatusBarChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register status bar event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_event_handler_register(UI_EVENT, UiEvents::kNotificationTextChanged, &OnNotificationTextChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register notification text event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_event_handler_register(UI_EVENT, UiEvents::kOverlaysDismissed, &OnOverlaysDismissed, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register overlay event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_event_handler_register(UI_EVENT, UiEvents::kVolumeChanged, &OnVolumeChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register volume event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_event_handler_register(UI_EVENT, UiEvents::kLocalizedTextChanged, &OnLocalizedTextChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register localized text event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_event_handler_register(UI_EVENT, UiEvents::kWifiConfigHintChanged, &OnWifiConfigHintChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi config hint handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_register(UI_EVENT, UiEvents::kPowerSaveModeChanged, &OnPowerSaveModeChanged, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register power save mode handler: %s", esp_err_to_name(ret));
        return ret;
    }

    initialized = true;
    return ESP_OK;
}

}  // namespace UiEventHandler

