#include "application.h"

#include "boards/common/board.h"
#include "boards/common/system_reset.h"

#include <esp_log.h>

namespace {
constexpr char kTag[] = "ApplicationButton";
}

void Application::OnButtonEvent(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    (void)event_base;
    auto* app = static_cast<Application*>(arg);
    if (app == nullptr || event_data == nullptr) {
        return;
    }
    const auto event = *static_cast<const ButtonEvents::Event*>(event_data);
    app->Schedule([app, event_id, count = event.click_count]() {
        app->HandleButtonEvent(event_id, count);
    });
}

void Application::HandleButtonEvent(int32_t event_id, uint8_t click_count) {
    switch (event_id) {
        case ButtonEvents::kShortPress1:
            HandleBoardInput({BoardEvents::Input::PrimaryClick, 0, false});
            break;
        case ButtonEvents::kShortPress2:
        case ButtonEvents::kShortPress4:
            // Reserved for future button actions.
            break;
        case ButtonEvents::kShortPress3: {
            auto mode = GetDuplexMode();
            DuplexMode next = mode == DuplexMode::kSimplex ? DuplexMode::kHalfDuplex :
                              mode == DuplexMode::kHalfDuplex ? DuplexMode::kFullDuplex :
                                                               DuplexMode::kSimplex;
            if (next == DuplexMode::kFullDuplex && !audio_service_.SupportsAec()) {
                next = DuplexMode::kSimplex;
            }
            SetDuplexMode(next);
            break;
        }
        case ButtonEvents::kShortPress5To7:
            if (click_count == 5) {
                ClearNetworkConfiguration();
            }
            break;
        case ButtonEvents::kShortPress8OrMore:
            StartFactoryReset();
            break;
        default:
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
    if (!Board::GetInstance().ClearNetworkConfiguration()) {
        ESP_LOGW(kTag, "Current board does not support Wi-Fi configuration reset");
        return;
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        SetDeviceState(kDeviceStateIdle);
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
    }
    ResetXiaolingConn();
}

void Application::StartFactoryReset() {
    if (factory_reset_started_.exchange(true)) {
        return;
    }
    ESP_LOGW(kTag, "Detected 8 or more clicks; factory reset requested");
    // NVS and OTA metadata are erased only after the prompt; reboot follows.
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
    // Playback is asynchronous. Bound the initial wait in case the tone is absent.
    for (int i = 0; session_id != 0 && i < 50 && !audio_player_.IsPlaying(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (session_id != 0 && audio_player_.IsPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    SystemReset::FactoryReset();
    vTaskDelete(nullptr);
}
