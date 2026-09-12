#include "time_sync.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>

#include <ctime>

#define TAG "TimeSync"

namespace TimeSync {

bool IsSystemTimeValid() {
    const auto now = std::time(nullptr);
    std::tm utc_time = {};
    return gmtime_r(&now, &utc_time) != nullptr && utc_time.tm_year >= (2024 - 1900);
}

bool SyncSystemTime() {
    if (IsSystemTimeValid()) {
        ESP_LOGI(TAG, "系统时间已经有效，无需重复校时");
        return true;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    const auto init_result = esp_netif_sntp_init(&config);
    const bool owns_sntp = init_result == ESP_OK;
    if (init_result != ESP_OK && init_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SNTP 初始化失败: %s", esp_err_to_name(init_result));
        return false;
    }

    const auto sync_result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    if (owns_sntp) {
        esp_netif_sntp_deinit();
    }
    if (sync_result != ESP_OK || !IsSystemTimeValid()) {
        ESP_LOGE(TAG, "SNTP 校时失败: %s", esp_err_to_name(sync_result));
        return false;
    }

    ESP_LOGI(TAG, "SNTP 校时完成");
    return true;
}

}  // TimeSync 命名空间
