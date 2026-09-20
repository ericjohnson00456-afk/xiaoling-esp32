#include "localized_audio.h"

#include "resource/store/asset_store.h"
#include "ui/i18n/lang_config.h"

LocalizedAudio& LocalizedAudio::GetInstance() {
    static LocalizedAudio instance;
    return instance;
}

bool LocalizedAudio::Initialize(AssetStore& assets) {
    assets_ = &assets;
    return true;
}

bool LocalizedAudio::GetPathData(const std::string& path,
                                 const uint8_t** data, size_t* size) const {
    if (assets_ == nullptr || data == nullptr || size == nullptr) {
        return false;
    }
    void* raw = nullptr;
    size_t length = 0;
    if (!assets_->GetAssetData(path, raw, length) || raw == nullptr || length == 0) {
        return false;
    }
    *data = static_cast<const uint8_t*>(raw);
    *size = length;
    return true;
}

bool LocalizedAudio::GetData(const std::string& name,
                             const uint8_t** data, size_t* size) const {
    if (name.empty() || assets_ == nullptr) {
        return false;
    }
    std::string filename = name + ".ogg";
    const bool activation_asset = name == "activation" ||
                                  (name.size() == 1 && name[0] >= '0' && name[0] <= '9');
    if (name == "activation") {
        filename = "300_activation.ogg";
    } else if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
        filename = std::to_string(kActivationDigitFirstId + (name[0] - '0')) +
                   "_" + name + ".ogg";
    }
    const std::string localized_path = activation_asset
        ? std::string("/activation/") + Lang::CODE + "/" + filename
        : std::string("/locales/") + Lang::CODE + "/" + filename;
    if (GetPathData(localized_path, data, size)) {
        return true;
    }

    const std::string fallback_path = activation_asset
        ? std::string("/activation/en-US/") + filename
        : std::string("/locales/en-US/") + filename;
    if (GetPathData(fallback_path, data, size)) {
        return true;
    }

    if (activation_asset) {
        // Keep packages generated before the 300-series resource namespace
        // readable during a rolling firmware/resource update.
        const std::string legacy_path = std::string("/locales/") + Lang::CODE + "/" + name + ".ogg";
        if (GetPathData(legacy_path, data, size)) {
            return true;
        }
        return GetPathData(std::string("/locales/en-US/") + name + ".ogg", data, size);
    }
    return false;
}

bool LocalizedAudio::GetActivationData(uint16_t id,
                                       const uint8_t** data, size_t* size) const {
    if (id == kActivationGuideId) {
        return GetData("activation", data, size);
    }
    if (id < kActivationDigitFirstId || id > kActivationDigitLastId) {
        return false;
    }
    const char digit = static_cast<char>('0' + id - kActivationDigitFirstId);
    return GetData(std::string(1, digit), data, size);
}
