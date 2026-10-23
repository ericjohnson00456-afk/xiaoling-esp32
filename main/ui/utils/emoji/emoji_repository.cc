#include "emoji_repository.h"

#include "resource/resource.h"
#include "ui/utils/image/rgb565_pixels.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <src/draw/lv_image_decoder_private.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#define TAG "EmojiRepository"

namespace {

constexpr uint32_t kDefaultIntervalMs = 100;
// Do not spend the audio/network allocation headroom on decoded UI resources.
constexpr size_t kEmojiPsramReserveBytes = 3 * 1024 * 1024;

struct FrameLoader {
    std::map<const uint8_t*, std::shared_ptr<LvglImage>> frames;
    size_t decoded_bytes = 0;
    size_t decoded_frames = 0;
    size_t raw_frames = 0;

    std::shared_ptr<LvglImage> Load(const ResourceData& asset) {
        const auto cached = frames.find(asset.address);
        if (cached != frames.end()) {
            return cached->second;
        }

        auto raw = std::make_shared<LvglRawImage>(
            const_cast<uint8_t*>(asset.address), asset.size);
        std::shared_ptr<LvglImage> image;
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
        // This style has a fixed black background. Flattening alpha here also
        // removes per-pixel alpha blending from the animation render path.
        lv_image_header_t header{};
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > kEmojiPsramReserveBytes &&
            lv_image_decoder_get_info(raw->image_dsc(), &header) == LV_RESULT_OK &&
            header.w != 0 && header.h != 0 &&
            header.w <= std::numeric_limits<uint32_t>::max() / 2 / header.h) {
            const size_t pixels = static_cast<size_t>(header.w) * header.h;
            const size_t bytes = pixels * 2;
            // Account for both the resident RGB565 image and temporary BGRA
            // decode buffer before opening the PNG. Never fall back to SRAM.
            const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            if (bytes <= std::numeric_limits<size_t>::max() / 3 &&
                free_psram > kEmojiPsramReserveBytes &&
                bytes * 3 < free_psram - kEmojiPsramReserveBytes) {
                image = Decode(*raw, header.w, header.h, bytes);
                if (image != nullptr) {
                    decoded_bytes += bytes;
                    ++decoded_frames;
                    // Loading all animations can take seconds at startup.
                    // Let the idle task run between frames, including at OTA reload.
                    vTaskDelay(1);
                }
            }
        }
#endif
        if (image == nullptr) {
            image = std::move(raw);
            ++raw_frames;
        }
        frames.emplace(asset.address, image);
        return image;
    }

private:
    static std::shared_ptr<LvglImage> Decode(const LvglImage& source,
                                            uint32_t width, uint32_t height,
                                            size_t bytes) {
        // LVGL 9 exposes the decoder session fields in this private header.
        // no_cache keeps the transient BGRA image out of the shared image cache;
        // the repository owns the smaller RGB565 allocation until its last user exits.
        struct DecoderSession {
            lv_image_decoder_dsc_t descriptor{};
            ~DecoderSession() { lv_image_decoder_close(&descriptor); }
        } session;
        lv_image_decoder_args_t args{};
        args.no_cache = true;
        if (lv_image_decoder_open(&session.descriptor, source.image_dsc(), &args) != LV_RESULT_OK) {
            ESP_LOGW(TAG, "表情预解码失败，保留原图");
            return nullptr;
        }
        const auto* decoded = session.descriptor.decoded;
        if (decoded == nullptr || decoded->data == nullptr ||
            decoded->header.cf != LV_COLOR_FORMAT_ARGB8888 ||
            decoded->header.w != width || decoded->header.h != height ||
            decoded->header.stride < width * 4) {
            ESP_LOGW(TAG, "表情解码格式不支持预转换，保留原图");
            return nullptr;
        }
        auto* pixels = static_cast<uint8_t*>(
            heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (pixels == nullptr) {
            ESP_LOGW(TAG, "表情像素 PSRAM 分配失败: bytes=%u", static_cast<unsigned>(bytes));
            return nullptr;
        }
        BgraOnBlackToRgb565(decoded->data, decoded->header.stride, width, height, pixels);
        return std::make_shared<LvglAllocatedImage>(
            pixels, bytes, width, height, width * 2, LV_COLOR_FORMAT_RGB565);
    }
};

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

bool GetEmojiAsset(std::string_view path, ResourceData& asset) {
    if (Resource::Get(ResourcePackage::Emoji, path, asset)) {
        return true;
    }
    constexpr std::string_view kLegacyPrefix = "/emoji/";
    if (path.substr(0, kLegacyPrefix.size()) == kLegacyPrefix) {
        return Resource::Get(ResourcePackage::Emoji,
                                  path.substr(kLegacyPrefix.size()), asset);
    }
    return false;
}

bool ParsePhase(const cJSON* json, EmojiPhase& phase, bool loop, FrameLoader& loader) {
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

        ResourceData asset;
        if (!GetEmojiAsset(path, asset)) {
            ESP_LOGE(TAG, "找不到动画帧: %s", path);
            return false;
        }
        phase.frames.emplace_back(loader.Load(asset));
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

void EmojiRepository::AddEmoji(const std::string& name, LvglImage* image) {
    emojis_[name].reset(image);
}

const LvglImage* EmojiRepository::GetEmojiImage(const char* name) const {
    if (name == nullptr) {
        return nullptr;
    }
    const auto it = emojis_.find(name);
    return it == emojis_.end() ? nullptr : it->second.get();
}

void EmojiRepository::ClearAnimations() {
    animations_.clear();
    default_emoji_.clear();
}

bool EmojiRepository::LoadFromResources(const std::string& config_path) {
    ClearAnimations();
    const int64_t started_us = esp_timer_get_time();
    FrameLoader loader;

    ResourceData asset;
    if (!Resource::Get(ResourcePackage::Emoji, config_path, asset)) {
        // Split emoji.bin packages keep emoji.json at the ROMFS root, while
        // legacy assets.bin used /emoji/emoji.json.
        const std::string fallback = config_path == "/emoji.json" ? "/emoji/emoji.json" : "/emoji.json";
        if (!Resource::Get(ResourcePackage::Emoji, fallback, asset)) {
            ESP_LOGE(TAG, "找不到表情配置: %s", config_path.c_str());
            return false;
        }
    }

    cJSON* root = cJSON_ParseWithLength(
        reinterpret_cast<const char*>(asset.address), asset.size);
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

        if (!ParsePhase(cJSON_GetObjectItemCaseSensitive(item, "intro"), animation.intro, false, loader) ||
            !ParsePhase(cJSON_GetObjectItemCaseSensitive(item, "loop"), animation.loop, true, loader) ||
            !ParsePhase(cJSON_GetObjectItemCaseSensitive(item, "outro"), animation.outro, false, loader)) {
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
    ESP_LOGI(TAG, "表情预解码: RGB565=%u 帧, pixels=%u bytes, 原图=%u 帧, elapsed=%lums",
             static_cast<unsigned>(loader.decoded_frames),
             static_cast<unsigned>(loader.decoded_bytes),
             static_cast<unsigned>(loader.raw_frames),
             static_cast<unsigned long>((esp_timer_get_time() - started_us) / 1000));
    return success;
}

void EmojiRepository::PredecodeRemainingFrames() {
    FrameLoader loader;
    for (auto& entry : animations_) {
        for (auto* phase : {&entry.second.intro, &entry.second.loop, &entry.second.outro}) {
            for (auto& frame : phase->frames) {
                const auto* source = frame->image_dsc();
                if (source->header.cf == LV_COLOR_FORMAT_RAW ||
                    source->header.cf == LV_COLOR_FORMAT_RAW_ALPHA) {
                    frame = loader.Load(ResourceData{source->data, source->data_size});
                }
            }
        }
    }
    if (loader.decoded_frames != 0 || loader.raw_frames != 0) {
        ESP_LOGI(TAG, "表情补充预解码: RGB565=%u 帧, pixels=%u bytes, 剩余原图=%u 帧",
                 static_cast<unsigned>(loader.decoded_frames),
                 static_cast<unsigned>(loader.decoded_bytes),
                 static_cast<unsigned>(loader.raw_frames));
    }
}

const EmojiAnimation* EmojiRepository::GetAnimation(const std::string& name) const {
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

std::vector<std::string> EmojiRepository::GetNames() const {
    std::vector<std::string> names;
    names.reserve(animations_.size());
    for (const auto& item : animations_) {
        names.push_back(item.first);
    }
    return names;
}
