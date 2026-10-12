#include "time_sync.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>

#include <ctime>
#include <mutex>

#define TAG "TimeSync"

namespace TimeSync {

namespace {

constexpr uint32_t kSyncIntervalMs = 60U * 60U * 1000U;
constexpr uint32_t kInitialSyncAttempts = 4;
constexpr uint32_t kInitialSyncWaitMs = 5000;
std::mutex s_sync_mutex;
bool s_sntp_initialized = false;

void OnTimeSynchronized(struct timeval*) {
    ESP_LOGI(TAG, "SNTP 周期校时完成");
}

}  // namespace

bool IsSystemTimeValid() {
    const auto now = std::time(nullptr);
    std::tm utc_time = {};
    return gmtime_r(&now, &utc_time) != nullptr && utc_time.tm_year >= (2024 - 1900);
}

bool SyncSystemTime() {
    std::lock_guard<std::mutex> lock(s_sync_mutex);

    bool first_start = false;
    if (!s_sntp_initialized) {
        esp_sntp_set_sync_interval(kSyncIntervalMs);
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            4, ESP_SNTP_SERVER_LIST(
                   "ntp.aliyun.com", "ntp.tencent.com", "time.cloudflare.com",
                   "time.google.com"));
        config.sync_cb = OnTimeSynchronized;
        const auto init_result = esp_netif_sntp_init(&config);
        if (init_result != ESP_OK && init_result != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "SNTP 初始化失败: %s", esp_err_to_name(init_result));
            return false;
        }
        s_sntp_initialized = true;
        first_start = true;
        ESP_LOGI(TAG,
                 "SNTP 常驻校时已启动，周期=%u ms，候选节点=ntp.aliyun.com, "
                 "ntp.tencent.com, time.cloudflare.com, time.google.com",
                 static_cast<unsigned>(kSyncIntervalMs));
    }

    if (!first_start && IsSystemTimeValid()) {
        return true;
    }

    for (uint32_t attempt = 1; attempt <= kInitialSyncAttempts; ++attempt) {
        const auto sync_result = esp_netif_sntp_sync_wait(
            pdMS_TO_TICKS(kInitialSyncWaitMs));
        if (sync_result == ESP_OK) break;
        ESP_LOGW(TAG, "SNTP 校时重试 %u/%u 超时，等待切换候选节点",
                 static_cast<unsigned>(attempt),
                 static_cast<unsigned>(kInitialSyncAttempts));
        if (attempt == kInitialSyncAttempts) {
            ESP_LOGE(TAG, "SNTP 校时失败: %s", esp_err_to_name(sync_result));
            // 服务保持运行并会自动重试；RTC 已有可信时间时不阻断启动链路。
            return IsSystemTimeValid();
        }
    }
    if (!IsSystemTimeValid()) {
        ESP_LOGE(TAG, "SNTP 返回成功，但系统时间仍无效");
        return false;
    }

    ESP_LOGI(TAG, "SNTP 首次校时完成，后续每小时自动校准");
    return true;
}

}  // TimeSync 命名空间
