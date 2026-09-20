#include "application.h"

#include "boards/common/board.h"
#include "system/system_reset.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"
#include "ui/ui_app.h"
#include "ui/screens/qrcode/qrcode_presenter.h"

#include <esp_log.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

namespace {
constexpr char kTag[] = "ApplicationButton";
constexpr uint16_t kProvisioningToneId = 70;
constexpr uint16_t kDeviceConfigToneId = 104;
}

void Application::HandleButtonClick(uint8_t click_count) {
    Schedule([this, click_count]() {
        ProcessButtonClick(click_count);
    });
}

void Application::HandleButtonLongPress() {
    Schedule([this]() {
        ProcessButtonLongPress();
    });
}

void Application::ProcessButtonLongPress() {
    if (!ota_confirmation_pending_.load()) {
        return;
    }

    ESP_LOGI(kTag, "Long press: skip pending app OTA");
    ota_confirmation_decision_.store(-1);
    audio_player_.Stop(PlaybackEndReason::kCancelled);
    xEventGroupSetBits(event_group_, MAIN_EVENT_OTA_CONFIRM_SKIP);
}

void Application::ProcessButtonClick(uint8_t click_count) {
    if (click_count == 1 && ota_confirmation_pending_.load()) {
        ESP_LOGI(kTag, "Single click: start pending app OTA");
        ota_confirmation_decision_.store(1);
        audio_player_.Stop(PlaybackEndReason::kCancelled);
        xEventGroupSetBits(event_group_, MAIN_EVENT_OTA_CONFIRM_START);
        return;
    }

    switch (click_count) {
        case 1:
            if (UiManager::GetInstance().lvgl_display() != nullptr &&
                UiManager::GetInstance().TopScreenId() != UI_PAGE_HOME) {
                UiApp::NavigateHome();
                break;
            }
            if (GetDeviceState() == kDeviceStateStarting &&
                Board::GetInstance().EnterNetworkConfigMode()) {
                return;
            }
            ToggleChatState();
            break;
        case 2:
#if CONFIG_USE_DEVICE_AEC
            if (GetDeviceState() == kDeviceStateIdle) {
                SetAecMode(GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
#endif
            break;
        case 3: {
            auto& qrcode_presenter = QrCodePresenter::GetInstance();
            const bool has_role_config_qrcode = qrcode_presenter.HasRoleConfigQrCode();
            qrcode_presenter.SelectRoleConfig();
            UiApp::NavigateToQrCode();
            PlayTone(has_role_config_qrcode ? kDeviceConfigToneId : kProvisioningToneId);
            break;
        }
        case 4:
            UiApp::ToggleLanguage();
            break;
        case 5:
            ClearNetworkConfiguration();
            break;
        default:
            if (click_count >= 8) {
                StartFactoryReset();
            }
            break;
    }
}

void Application::ClearNetworkConfiguration() {
    const auto state = GetDeviceState();
    if (state == kDeviceStateUpgrading || state == kDeviceStateFatalError) {
        ESP_LOGW(kTag, "Cannot clear network configuration in state %s",
                 DeviceStatus::GetStateName(state));
        return;
    }
    WifiManager::GetInstance().StopStation();
    SsidManager::GetInstance().Clear();
    if (!Board::GetInstance().EnterNetworkConfigMode()) {
        ESP_LOGW(kTag, "Current board does not support Wi-Fi configuration reset");
        return;
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        SetDeviceState(kDeviceStateIdle);
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
    }
    SetDeviceState(kDeviceStateWifiConfiguring);
    ResetXiaolingConn();
}

void Application::StartFactoryReset() {
    if (factory_reset_started_.exchange(true)) {
        return;
    }
    ESP_LOGW(kTag, "Detected 8 or more clicks; factory reset requested");
    if (xTaskCreate([](void* arg) {
            static_cast<Application*>(arg)->FactoryResetTask();
        }, "factory_reset", 4096, this, 5, nullptr) != pdPASS) {
        factory_reset_started_ = false;
        ESP_LOGE(kTag, "Failed to create factory reset task");
    }
}

void Application::FactoryResetTask() {
    audio_player_.Stop(PlaybackEndReason::kCancelled);
    const auto session_id = audio_player_.PlayTone(103);
    for (int i = 0; session_id != 0 && i < 50 && !audio_player_.IsPlaying(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (session_id != 0 && audio_player_.IsPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    SystemReset::ResetToFactory();
    vTaskDelete(nullptr);
}
