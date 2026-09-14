#include "tone_model.h"

#include "resource/store/asset_store.h"
#include "ui/i18n/lang_config.h"

#include <esp_log.h>

#include <cctype>
#include <cstdio>

#define TAG "ToneModel"

ToneModel& ToneModel::GetInstance() {
    static ToneModel instance;
    return instance;
}

bool ToneModel::Initialize(AssetStore& assets, const std::string& directory) {
    assets_ = &assets;
    urls_.clear();
    items_.clear();
    initialized_ = false;

    std::vector<std::string> paths;
    if (!assets.ListAssetFiles(directory, paths)) {
        ESP_LOGW(TAG, "ROMFS 中没有 tone 目录: %s", directory.c_str());
        return false;
    }

    struct PendingTone {
        uint16_t id;
        std::string path;
        const uint8_t* data;
        size_t size;
    };
    std::vector<PendingTone> pending;
    uint16_t max_id = 0;

    for (const auto& path : paths) {
        const size_t slash = path.find_last_of('/');
        const std::string filename = slash == std::string::npos ? path : path.substr(slash + 1);
        if (filename.size() < 5 || !std::isdigit(static_cast<unsigned char>(filename[0])) ||
            !std::isdigit(static_cast<unsigned char>(filename[1])) ||
            !std::isdigit(static_cast<unsigned char>(filename[2])) || filename[3] != '_') {
            ESP_LOGW(TAG, "忽略无效 tone 文件名: %s", path.c_str());
            continue;
        }

        const uint16_t id = static_cast<uint16_t>((filename[0] - '0') * 100 +
                                                   (filename[1] - '0') * 10 + filename[2] - '0');
        void* data = nullptr;
        size_t size = 0;
        if (!assets.GetAssetData(path, data, size) || data == nullptr || size == 0) {
            ESP_LOGW(TAG, "找不到 tone 数据: %s", path.c_str());
            continue;
        }
        pending.push_back({id, path, static_cast<const uint8_t*>(data), size});
        if (id > max_id) {
            max_id = id;
        }
    }

    if (pending.empty()) {
        return false;
    }

    urls_.resize(static_cast<size_t>(max_id) + 1);
    items_.resize(static_cast<size_t>(max_id) + 1);
    for (const auto& tone : pending) {
        char url[64] = {};
        const size_t slash = tone.path.find_last_of('/');
        const std::string filename = slash == std::string::npos ? tone.path : tone.path.substr(slash + 1);
        std::snprintf(url, sizeof(url), "embed://tone/%03u_%s", tone.id, filename.substr(4).c_str());
        urls_[tone.id] = url;
        items_[tone.id] = {
            .address = tone.data,
            .size = static_cast<int>(tone.size),
        };
    }

    initialized_ = true;
    ESP_LOGI(TAG, "ROMFS tone 加载完成: %u 个，最大 ID=%u",
             static_cast<unsigned>(pending.size()), static_cast<unsigned>(max_id));
    return true;
}

const char* ToneModel::GetUrl(uint16_t tone_id) const {
    if (!initialized_ || tone_id >= urls_.size() || urls_[tone_id].empty()) {
        return nullptr;
    }
    return urls_[tone_id].c_str();
}

bool ToneModel::GetLocalizedData(const std::string& name, const uint8_t** data, size_t* size) const {
    if (!initialized_ || data == nullptr || size == nullptr || name.empty()) {
        return false;
    }
    if (assets_ == nullptr) {
        return false;
    }
    const std::string localized_path = std::string("/locales/") + Lang::CODE + "/" + name + ".ogg";
    void* raw = nullptr;
    size_t length = 0;
    if (!assets_->GetAssetData(localized_path, raw, length)) {
        const std::string fallback_path = std::string("/locales/en-US/") + name + ".ogg";
        if (!assets_->GetAssetData(fallback_path, raw, length)) {
            return false;
        }
    }
    *data = static_cast<const uint8_t*>(raw);
    *size = length;
    return true;
}
