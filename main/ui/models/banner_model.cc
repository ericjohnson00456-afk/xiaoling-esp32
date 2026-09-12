#include "banner_model.h"

#include <cstring>
#include <utility>

#include <esp_log.h>

#define TAG "BannerModel"

// 服务端未给 interval_ms 时的默认轮换间隔，以及可接受的最小间隔（防止异常值刷屏）
#define BANNER_DEFAULT_INTERVAL_MS 3000
#define BANNER_MIN_INTERVAL_MS     1000

// 云端横幅文本中的唤醒词占位符
static constexpr const char* kWakeWordPlaceholder = "#唤醒词#";

void BannerModel::HandleSettingMessage(const cJSON* data) {
    const auto* theme = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "theme") : nullptr;
    const auto* frontend = cJSON_IsObject(theme) ? cJSON_GetObjectItemCaseSensitive(theme, "frontend") : nullptr;
    const auto* banner = cJSON_IsObject(frontend) ? cJSON_GetObjectItemCaseSensitive(frontend, "banner") : nullptr;
    if (!cJSON_IsObject(banner)) {
        // 非 banner 内容（ttsProperties、mp_guide 等）由其他模块各自消费
        return;
    }

    uint32_t interval_ms = BANNER_DEFAULT_INTERVAL_MS;
    const auto* interval = cJSON_GetObjectItemCaseSensitive(banner, "interval_ms");
    if (cJSON_IsNumber(interval) && interval->valueint >= BANNER_MIN_INTERVAL_MS) {
        interval_ms = static_cast<uint32_t>(interval->valueint);
    }

    std::vector<std::string> texts;
    const auto* resources = cJSON_GetObjectItemCaseSensitive(banner, "resources");
    if (cJSON_IsArray(resources)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, resources) {
            const auto* text = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "text") : nullptr;
            if (cJSON_IsString(text) && text->valuestring != nullptr && text->valuestring[0] != '\0') {
                texts.push_back(SubstituteWakeWord(text->valuestring));
            }
        }
    }

    ESP_LOGI(TAG, "Banner 更新: %u 条, 间隔 %ums", static_cast<unsigned>(texts.size()),
             static_cast<unsigned>(interval_ms));
    Publish(BannerModelState{
        .texts = std::move(texts),
        .interval_ms = interval_ms,
    });
}

std::string BannerModel::SubstituteWakeWord(const std::string& text) const {
    if (!wake_word_provider_ || text.find(kWakeWordPlaceholder) == std::string::npos) {
        return text;
    }
    const std::string wake_word = wake_word_provider_();
    if (wake_word.empty()) {
        // 尚未唤醒过、拿不到唤醒词名称时保留原文，由下次推送自然覆盖
        return text;
    }

    std::string result = text;
    size_t pos = 0;
    while ((pos = result.find(kWakeWordPlaceholder, pos)) != std::string::npos) {
        result.replace(pos, strlen(kWakeWordPlaceholder), wake_word);
        pos += wake_word.size();
    }
    return result;
}
