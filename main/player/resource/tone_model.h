#pragma once

#include <esp_gmf_io_embed_flash.h>

#include <cstdint>
#include <string>
#include <vector>

class AssetStore;

// ROMFS tone index. File names use a three-digit numeric prefix, for example
// /tone/059_network_suc.mp3.
class ToneModel final {
public:
    static ToneModel& GetInstance();

    bool Initialize(AssetStore& assets, const std::string& directory = "/tone");
    const char* GetUrl(uint16_t tone_id) const;

    const embed_item_info_t* EmbedItems() const { return items_.data(); }
    int EmbedItemCount() const { return static_cast<int>(items_.size()); }
    bool initialized() const { return initialized_; }

private:
    ToneModel() = default;
    ToneModel(const ToneModel&) = delete;
    ToneModel& operator=(const ToneModel&) = delete;

    std::vector<std::string> urls_;
    std::vector<embed_item_info_t> items_;
    bool initialized_ = false;
};
