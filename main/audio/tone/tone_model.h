#pragma once

#include <esp_gmf_io_embed_flash.h>

#include <cstdint>
#include <string>
#include <vector>

class AssetStore;

// ROMFS tone 索引。文件名使用三位数字前缀，例如 /tone/059_network_suc.mp3。
class ToneModel final {
public:
    static ToneModel& GetInstance();

    bool Initialize(AssetStore& assets, const std::string& directory = "/tone");
    const char* GetUrl(uint16_t tone_id) const;
    // 按逻辑名称读取当前语言 OGG，找不到时回退到 en-US。
    bool GetLocalizedData(const std::string& name, const uint8_t** data, size_t* size) const;

    // 给 ESP Audio Simple Player 的 embed_flash IO 使用。
    const embed_item_info_t* EmbedItems() const { return items_.data(); }
    int EmbedItemCount() const { return static_cast<int>(items_.size()); }
    bool initialized() const { return initialized_; }

private:
    ToneModel() = default;
    ToneModel(const ToneModel&) = delete;
    ToneModel& operator=(const ToneModel&) = delete;

    std::vector<std::string> urls_;
    std::vector<embed_item_info_t> items_;
    AssetStore* assets_ = nullptr;
    bool initialized_ = false;
};
