#include "emoji_model.h"

#include "resource/assets.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstdio>
#include <utility>

#define TAG "EmojiModel"

namespace {

constexpr uint32_t kDefaultIntervalMs = 100;

bool GetArrayInt(const cJSON* array, int index, int* value) {
    const cJSON* item = cJSON_GetArrayItem(array, index);
    if (item == nullptr || !cJSON_IsNumber(item) || value == nullptr) {
        return false;
    }
    *value = item->valueint;
    return true;
}

bool GetObjectInt(const cJSON* object, const char* key, int* value) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || value == nullptr) {
        return false;
    }
    *value = item->valueint;
    return true;
}

bool ParsePhase(const cJSON* json, Assets& assets, EmojiPhase& phase, bool loop) {
    if (json == nullptr) {
        return true;
    }
    if (!cJSON_IsObject(json)) {
        ESP_LOGE(TAG, "动画阶段不是对象");
        return false;
    }

    const cJSON* path_json = cJSON_GetObjectItemCaseSensitive(json, "path");
    const cJSON* range_json = cJSON_GetObjectItemCaseSensitive(json, "range");
    if (!cJSON_IsString(path_json) || path_json->valuestring == nullptr ||
        !cJSON_IsArray(range_json) || cJSON_GetArraySize(range_json) != 2) {
        ESP_LOGE(TAG, "动画阶段缺少 path 或 range");
        return false;
    }

    int frame_begin = 0;
    int frame_end = 0;
    if (!GetArrayInt(range_json, 0, &frame_begin) || !GetArrayInt(range_json, 1, &frame_end) ||
        frame_begin < 0 || frame_end < frame_begin) {
        ESP_LOGE(TAG, "动画帧范围无效");
        return false;
    }

    int interval = static_cast<int>(kDefaultIntervalMs);
    if (GetObjectInt(json, "interval_ms", &interval) && interval > 0) {
        phase.default_interval_ms = static_cast<uint32_t>(interval);
    } else {
        phase.default_interval_ms = kDefaultIntervalMs;
    }
    phase.loop = loop ? -1 : 0;

    const int frame_count = frame_end - frame_begin + 1;
    phase.frames.reserve(frame_count);
    phase.delays.assign(frame_count, phase.default_interval_ms);

    for (int frame = frame_begin; frame <= frame_end; ++frame) {
        char path[512] = {};
        const int path_len = std::snprintf(path, sizeof(path), path_json->valuestring, frame);
        if (path_len <= 0 || path_len >= static_cast<int>(sizeof(path))) {
            ESP_LOGE(TAG, "动画资源路径过长或格式无效: %s", path_json->valuestring);
            return false;
        }

        void* data = nullptr;
        size_t size = 0;
        if (!assets.GetAssetData(path, data, size) || data == nullptr || size == 0) {
            ESP_LOGE(TAG, "找不到动画帧: %s", path);
            return false;
        }
        // ROMFS 数据映射在 Flash 中，LvglRawImage 只保存描述信息，不释放 data。
        phase.frames.emplace_back(std::make_shared<LvglRawImage>(data, size));
    }

    const cJSON* overrides = cJSON_GetObjectItemCaseSensitive(json, "interval_overrides");
    if (overrides != nullptr) {
        if (!cJSON_IsArray(overrides)) {
            ESP_LOGE(TAG, "interval_overrides 必须是数组");
            return false;
        }
        for (int i = 0; i < cJSON_GetArraySize(overrides); ++i) {
            const cJSON* item = cJSON_GetArrayItem(overrides, i);
            int frame = 0;
            int delay = 0;
            if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 2 ||
                !GetArrayInt(item, 0, &frame) || !GetArrayInt(item, 1, &delay) || delay < 0) {
                ESP_LOGE(TAG, "interval_overrides[%d] 格式无效", i);
                return false;
            }

            // 优先按绝对帧号处理，同时兼容旧的段内下标格式。
            int local_index = frame - frame_begin;
            if (local_index < 0 || local_index >= frame_count) {
                local_index = frame;
            }
            if (local_index < 0 || local_index >= frame_count) {
                ESP_LOGE(TAG, "interval_overrides[%d] 帧号越界: %d", i, frame);
                return false;
            }
            phase.delays[local_index] = static_cast<uint32_t>(delay);
        }
    }
    return true;
}

}  // namespace

void EmojiModel::AddEmoji(const std::string& name, LvglImage* image) {
    emojis_[name].reset(image);
}

const LvglImage* EmojiModel::GetEmojiImage(const char* name) const {
    if (name == nullptr) {
        return nullptr;
    }
    const auto it = emojis_.find(name);
    return it == emojis_.end() ? nullptr : it->second.get();
}

void EmojiModel::ClearAnimations() {
    animations_.clear();
    default_emoji_.clear();
}

bool EmojiModel::LoadFromAssets(Assets& assets, const std::string& config_path) {
    ClearAnimations();

    void* data = nullptr;
    size_t size = 0;
    if (!assets.GetAssetData(config_path, data, size) || data == nullptr || size == 0) {
        ESP_LOGE(TAG, "找不到表情配置: %s", config_path.c_str());
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(static_cast<const char*>(data), size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "表情配置 JSON 解析失败");
        return false;
    }

    const cJSON* default_json = cJSON_GetObjectItemCaseSensitive(root, "default_emoji");
    const cJSON* animations_json = cJSON_GetObjectItemCaseSensitive(root, "animations");
    if (!cJSON_IsString(default_json) || default_json->valuestring == nullptr ||
        !cJSON_IsArray(animations_json)) {
        ESP_LOGE(TAG, "表情配置缺少 default_emoji 或 animations");
        cJSON_Delete(root);
        return false;
    }
    default_emoji_ = default_json->valuestring;

    bool success = true;
    for (int i = 0; i < cJSON_GetArraySize(animations_json); ++i) {
        const cJSON* item = cJSON_GetArrayItem(animations_json, i);
        const cJSON* name_json = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsObject(item) || !cJSON_IsString(name_json) || name_json->valuestring == nullptr) {
            ESP_LOGE(TAG, "表情配置第 %d 项缺少 name", i);
            success = false;
            continue;
        }

        EmojiAnimation animation;
        animation.name = name_json->valuestring;
        const cJSON* alias_json = cJSON_GetObjectItemCaseSensitive(item, "alias");
        if (cJSON_IsString(alias_json) && alias_json->valuestring != nullptr) {
            animation.alias = alias_json->valuestring;
        }

        if (!ParsePhase(cJSON_GetObjectItemCaseSensitive(item, "intro"), assets, animation.intro, false) ||
            !ParsePhase(cJSON_GetObjectItemCaseSensitive(item, "loop"), assets, animation.loop, true) ||
            !ParsePhase(cJSON_GetObjectItemCaseSensitive(item, "outro"), assets, animation.outro, false)) {
            ESP_LOGE(TAG, "表情加载失败: %s", animation.name.c_str());
            success = false;
            continue;
        }
        animations_[animation.name] = std::move(animation);
    }

    cJSON_Delete(root);
    if (animations_.empty() || GetAnimation(default_emoji_) == nullptr) {
        ESP_LOGE(TAG, "没有可用表情或默认表情无效: %s", default_emoji_.c_str());
        return false;
    }
    ESP_LOGI(TAG, "表情动画加载完成: %u 个，默认=%s",
             static_cast<unsigned>(animations_.size()), default_emoji_.c_str());
    return success;
}

const EmojiAnimation* EmojiModel::GetAnimation(const std::string& name) const {
    std::string current = name;
    for (size_t depth = 0; depth <= animations_.size(); ++depth) {
        const auto it = animations_.find(current);
        if (it == animations_.end()) {
            return nullptr;
        }
        if (it->second.alias.empty()) {
            return &it->second;
        }
        current = it->second.alias;
    }
    ESP_LOGE(TAG, "表情 alias 存在循环: %s", name.c_str());
    return nullptr;
}
