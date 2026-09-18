#include "system_reset.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

namespace {
constexpr char kTag[] = "SystemReset";
constexpr int kRestartDelaySeconds = 1;
}

void SystemReset::ResetToFactory() {
    ResetNvsFlash();

    ESP_LOGI(kTag, "Resetting to factory");
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (partition == nullptr) {
        ESP_LOGE(kTag, "Failed to find otadata partition");
        return;
    }

    const esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to erase otadata partition: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(kTag, "Erased otadata partition");

    RestartInSeconds(kRestartDelaySeconds);
}

void SystemReset::ResetNvsFlash() {
    ESP_LOGI(kTag, "Resetting NVS flash");
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to erase NVS flash: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to initialize NVS flash: %s", esp_err_to_name(err));
    }
}

void SystemReset::RestartInSeconds(int seconds) {
    for (int i = seconds; i > 0; --i) {
        ESP_LOGI(kTag, "Resetting in %d seconds", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_restart();
}
