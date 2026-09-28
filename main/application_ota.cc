#include "application.h"

#include "board.h"
#include "resource/assets.h"
#include "settings.h"
#include "shell/cmd/kv/kv_user.h"
#include "system/time_sync.h"
#include "ui/i18n/lang_config.h"
#include "ui/ui_app.h"

#include <esp_log.h>

#include <cstdio>

namespace {

constexpr char kTag[] = "ApplicationOta";
constexpr EventBits_t kOtaConfirmStart = 1U << 13;
constexpr EventBits_t kOtaConfirmSkip = 1U << 14;

std::string FormatFirmwareSize(size_t size) {
    char text[32] = {};
    if (size >= 1024U * 1024U) {
        std::snprintf(text, sizeof(text), "%.1f MB",
                      static_cast<double>(size) / (1024.0 * 1024.0));
    } else if (size >= 1024U) {
        std::snprintf(text, sizeof(text), "%u KB",
                      static_cast<unsigned>(size / 1024U));
    } else {
        std::snprintf(text, sizeof(text), "%u B", static_cast<unsigned>(size));
    }
    return text;
}

}  // namespace

void Application::StartOtaFlow() {
    if (ota_flow_.task_handle != nullptr) {
        ESP_LOGW(kTag, "OTA flow is already running");
        return;
    }

    ota_flow_.resources_checked = false;
    ota_flow_.completed.store(false);
    ota_flow_.succeeded.store(false);
    ota_flow_.confirmation_pending.store(false);
    ota_flow_.confirmation_decision.store(0);
    SetDeviceState(kDeviceStateActivating);
    const BaseType_t result = xTaskCreate([](void* arg) {
        auto* app = static_cast<Application*>(arg);
        app->OtaTask();
        app->ota_flow_.task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "ota_flow", 4096 * 3, this, 2, &ota_flow_.task_handle);
    if (result != pdPASS) {
        ota_flow_.task_handle = nullptr;
        ESP_LOGE(kTag, "Failed to create OTA flow task");
        last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
        CompleteOtaFlow(false);
    }
}

void Application::CompleteOtaFlow(bool succeeded) {
    ESP_LOGI(kTag, "OTA flow completed: %s", succeeded ? "success" : "failed");
    ota_flow_.client.reset();
    ota_flow_.succeeded.store(succeeded);
    ota_flow_.completed.store(true);
    xEventGroupSetBits(event_group_, MAIN_EVENT_OTA_COMPLETED);
}

bool Application::HandleOtaConfirmation(bool confirm) {
    if (!ota_flow_.confirmation_pending.load()) {
        return false;
    }

    ESP_LOGI(kTag, "%s pending APP OTA", confirm ? "Confirming" : "Skipping");
    ota_flow_.confirmation_decision.store(confirm ? 1 : -1);
    audio_player_.Stop(PlaybackEndReason::kCancelled);
    xEventGroupSetBits(event_group_, confirm ? kOtaConfirmStart : kOtaConfirmSkip);
    return true;
}

void Application::OtaTask() {
    if (!TimeSync::SyncSystemTime()) {
        ESP_LOGE(kTag, "系统校时失败，停止本次 OTA 流程");
        last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
        CompleteOtaFlow(false);
        return;
    }

    auto& board = Board::GetInstance();
    ota_flow_.client = std::make_unique<Ota>();
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging) &&
        discharging && battery_level < 10) {
        ESP_LOGW(kTag, "Skip OTA on battery at %d%%", battery_level);
        PlayOtaTone(93);
        CompleteOtaFlow(true);
        return;
    }

    if (!CheckNewVersion()) {
        ESP_LOGE(kTag, "OTA 启动检查失败");
        last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
        CompleteOtaFlow(false);
        return;
    }

    const esp_err_t resource_check = ota_flow_.client->CheckResources();
    if (resource_check != ESP_OK && resource_check != ESP_ERR_NOT_FOUND) {
        // Resource metadata is optional during startup. No resource partition
        // has been modified at this point, so a transport/server failure is
        // safe to treat as "local resources are current" and startup can
        // continue. Failures after a resource download starts still reboot in
        // CheckAssetsVersion(), because those partitions are written in place.
        ESP_LOGW(kTag,
                 "Resource OTA metadata check failed (%s); treating local resources as latest and continuing",
                 esp_err_to_name(resource_check));
    }
    if (!CheckAssetsVersion()) {
        ESP_LOGE(kTag, "Resource OTA failed");
        last_error_message_ = Lang::Strings::DOWNLOAD_ASSETS_FAILED;
        CompleteOtaFlow(false);
        return;
    }

    CompleteOtaFlow(true);
}

bool Application::CheckAssetsVersion() {
    if (ota_flow_.resources_checked) {
        return true;
    }
    ota_flow_.resources_checked = true;

    auto& board = Board::GetInstance();
    auto& assets = Assets::GetInstance();
    if (!assets.RespakStore().partition_valid()) {
        ESP_LOGW(kTag, "Resource partitions are unavailable for board %s", BOARD_NAME);
        return true;
    }

    struct PendingResource {
        Ota::Resource resource;
        const char* label;
        const char* updating_text;
        const Ota::ResourceInfo& remote;
    };
    const PendingResource pending[] = {
        {Ota::Resource::Tone, "tone", Lang::Strings::UPDATING_TONE_RESOURCE,
         ota_flow_.client->GetToneResource()},
        {Ota::Resource::Emoji, "emoji", Lang::Strings::UPDATING_EMOJI_RESOURCE,
         ota_flow_.client->GetEmojiResource()},
        {Ota::Resource::WakeWord, "wake_word", Lang::Strings::UPDATING_WAKE_WORD_RESOURCE,
         ota_flow_.client->GetWakeWordResource()},
    };

    bool downloaded = false;
    bool resource_tone_played = false;
    bool tone_downloaded = false;
    bool emoji_downloaded = false;
    bool wake_word_downloaded = false;
    for (const auto& item : pending) {
        if ((item.resource == Ota::Resource::WakeWord &&
             KvOta::IsDisabled(KvOta::UpdateType::WakeWord)) ||
            (item.resource == Ota::Resource::Tone &&
             KvOta::IsDisabled(KvOta::UpdateType::Tone)) ||
            (item.resource == Ota::Resource::Emoji &&
             KvOta::IsDisabled(KvOta::UpdateType::Emoji))) {
            ESP_LOGI(kTag, "Skipping %s OTA: disabled by user KV", item.label);
            continue;
        }
        if (!item.remote.available) {
            continue;
        }
        if (ota_flow_.client->VerifyResource(item.resource, item.remote.size,
                                             item.remote.md5)) {
            continue;
        }

        ESP_LOGI(kTag, "Downloading %s resource from %s", item.label,
                 item.remote.url.c_str());
        SetDeviceState(kDeviceStateUpgrading);
        if (!resource_tone_played) {
            PlayOtaTone(89);
            resource_tone_played = true;
        }
        UiApp::ShowUpgradeProgress(item.updating_text, 0);
        if (item.resource == Ota::Resource::WakeWord) {
            // The wake-word model points directly into its mapped ROMFS image.
            // Stop inference before the partition is unmapped and rewritten.
            audio_service_.EnableWakeWordDetection(false);
        }
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        const bool success = ota_flow_.client->UpgradeResource(
            item.resource, item.remote.url, item.remote.size, item.remote.md5,
            [label = std::string(item.updating_text)](int progress, size_t speed) {
                UiApp::ShowUpgradeProgress(label, progress, speed);
            });
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        if (!success) {
            ESP_LOGE(kTag, "Failed to update %s resource; rebooting to retry", item.label);
            Reboot();
            return false;
        }
        downloaded = true;
        if (item.resource == Ota::Resource::Tone) {
            tone_downloaded = true;
        } else if (item.resource == Ota::Resource::Emoji) {
            emoji_downloaded = true;
        } else if (item.resource == Ota::Resource::WakeWord) {
            wake_word_downloaded = true;
        }
    }

    if (downloaded) {
        if (tone_downloaded && !assets.ReloadTone()) {
            ESP_LOGW(kTag, "Failed to reload tone model after tone update");
        }
        if (emoji_downloaded && !assets.ReloadEmoji()) {
            ESP_LOGW(kTag, "Failed to reload emoji resources after OTA");
        }
        if (wake_word_downloaded && !assets.ReloadWakeWord()) {
            ESP_LOGW(kTag, "Failed to reload wake-word resources after OTA");
        }
        PlayOtaTone(90);
        UiApp::ShowUpgradeResult(Lang::Strings::RESOURCE_UPDATE_SUCCESSFUL);
        vTaskDelay(pdMS_TO_TICKS(3000));
        Reboot();
        return true;
    }

    UiApp::SetChatMessage("system", "");
    UiApp::SetEmotion("microchip_ai");
    return true;
}

bool Application::CheckNewVersion() {
    UiApp::SetStatus(UiApp::Status::CheckingNewVersion);
    if (ota_flow_.client == nullptr) {
        return false;
    }
    const esp_err_t check_result = ota_flow_.client->CheckVersion();
    if (check_result != ESP_OK) {
        ESP_LOGW(kTag,
                 "APP OTA metadata check failed (%s); treating current firmware %s as latest and continuing",
                 esp_err_to_name(check_result), ota_flow_.client->GetCurrentVersion().c_str());
    }

    Settings ota_settings("ota", true);
    const bool app_update_succeeded = ota_settings.GetBool("app_update_pending_success", false);
    if (app_update_succeeded) {
        ota_settings.EraseKey("app_update_pending_success");
        PlayOtaTone(88);
    }
    if (!KvOta::IsDisabled(KvOta::UpdateType::App) &&
        ota_flow_.client->HasNewVersion()) {
        ESP_LOGI(kTag, "APP OTA available: %s, waiting for user confirmation",
                 ota_flow_.client->GetFirmwareVersion().c_str());

        const std::string description = ota_flow_.client->GetFirmwareDescription().empty()
                                            ? Lang::Strings::OTA_DEFAULT_NOTES
                                            : ota_flow_.client->GetFirmwareDescription();
        char confirmation[768] = {};
        if (ota_flow_.client->GetFirmwareSize() != 0) {
            std::snprintf(confirmation, sizeof(confirmation), Lang::Strings::OTA_CONFIRM_WITH_SIZE,
                          ota_flow_.client->GetFirmwareVersion().c_str(),
                          FormatFirmwareSize(ota_flow_.client->GetFirmwareSize()).c_str(),
                          description.c_str());
        } else {
            std::snprintf(confirmation, sizeof(confirmation), Lang::Strings::OTA_CONFIRM,
                          ota_flow_.client->GetFirmwareVersion().c_str(), description.c_str());
        }
        UiApp::SetStatusText(Lang::Strings::OTA_UPGRADE);
        UiApp::SetEmotion("download");
        UiApp::SetChatMessage("system", confirmation);

        ota_flow_.confirmation_decision.store(0);
        ota_flow_.confirmation_pending.store(true);
        PlayOtaTone(86, 3);
        const EventBits_t decision_bits = xEventGroupWaitBits(
            event_group_, kOtaConfirmStart | kOtaConfirmSkip,
            pdTRUE, pdFALSE, portMAX_DELAY);
        ota_flow_.confirmation_pending.store(false);
        ota_flow_.confirmation_decision.store(0);

        if ((decision_bits & kOtaConfirmStart) != 0) {
            ESP_LOGI(kTag, "User confirmed APP OTA: %s",
                     ota_flow_.client->GetFirmwareVersion().c_str());
            if (UpgradeFirmware(ota_flow_.client->GetFirmwareUrl(),
                                ota_flow_.client->GetFirmwareVersion())) {
                return false;
            }
            ESP_LOGW(kTag, "APP OTA failed; continuing with the current firmware");
        } else {
            ESP_LOGI(kTag, "User skipped APP OTA");
            UiApp::SetChatMessage("system", "");
        }
    }
    if (KvOta::IsDisabled(KvOta::UpdateType::App) &&
        ota_flow_.client->HasNewVersion()) {
        ESP_LOGI(kTag, "Skipping APP OTA: disabled by user KV");
    }
    ota_flow_.client->MarkCurrentVersionValid();
    return true;
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    const std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    ESP_LOGI(kTag, "Starting firmware upgrade from URL: %s", url.c_str());

    UiApp::SetStatusText(Lang::Strings::OTA_UPGRADE);
    UiApp::SetEmotion("download");
    UiApp::SetChatMessage("system", Lang::Strings::UPGRADING);
    PlayOtaTone(87);
    SetDeviceState(kDeviceStateUpgrading);

    UiApp::SetChatMessage("system", std::string(Lang::Strings::NEW_VERSION) + version_info);
    UiApp::ShowUpgradeProgress("APP", 0);

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    const bool upgrade_success = Ota::Upgrade(url, [](int progress, size_t speed) {
        UiApp::ShowUpgradeProgress("APP", progress, speed);
    });
    if (!upgrade_success) {
        ESP_LOGW(kTag, "Firmware upgrade interrupted; treating the current firmware as latest and continuing");
        audio_service_.Start();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        UiApp::SetChatMessage("system", "");
        UiApp::SetEmotion("microchip_ai");
        SetDeviceState(kDeviceStateActivating);
        return false;
    }

    ESP_LOGI(kTag, "Firmware upgrade successful, rebooting...");
    Settings ota_settings("ota", true);
    ota_settings.SetBool("app_update_pending_success", true);
    UiApp::SetChatMessage("system", Lang::Strings::UPGRADE_SUCCESSFUL_REBOOTING);
    vTaskDelay(pdMS_TO_TICKS(1000));
    Reboot();
    return true;
}

void Application::PlayOtaTone(uint16_t tone_id, uint8_t repeat_count) {
    for (uint8_t repeat = 0; repeat < repeat_count; ++repeat) {
        if (ota_flow_.confirmation_pending.load() &&
            ota_flow_.confirmation_decision.load() != 0) {
            return;
        }
        if (audio_player_.PlayTone(tone_id) == 0) {
            ESP_LOGW(kTag, "OTA tone playback failed: id=%u", static_cast<unsigned>(tone_id));
            return;
        }

        const TickType_t start = xTaskGetTickCount();
        bool started = false;
        while (xTaskGetTickCount() - start < pdMS_TO_TICKS(15000)) {
            if (ota_flow_.confirmation_pending.load() &&
                ota_flow_.confirmation_decision.load() != 0) {
                audio_player_.Stop(PlaybackEndReason::kCancelled);
                return;
            }
            if (audio_player_.IsPlaying()) {
                started = true;
            } else if (started) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
