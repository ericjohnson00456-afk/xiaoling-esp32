#ifndef ASSETS_H
#define ASSETS_H

#include "resource/store/asset_store.h"

#include <model_path.h>

// Xiaoling resource package management. Respak, tone, emoji and wake_word are
// independent ROMFS partitions. OTA download and flash writing belong to
// ota/ota_resource.cc.
class Assets {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }
    ~Assets() = default;

    // Load every runtime resource before UI pages are created. Wake-word model
    // installation is included, but detection is started later by AudioService.
    bool Apply();

    // Resource-specific reloads used after a successful in-place OTA update.
    bool ReloadTone();
    bool ReloadEmoji();
    bool ReloadWakeWord();

    // Load all runtime fonts from the respak/font directory.
    bool LoadFonts();
    AssetStore& RespakStore();
    AssetStore& ToneStore();
    AssetStore& EmojiStore();
    AssetStore& WakeWordStore();

private:
    Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    srmodel_list_t* models_list_ = nullptr;
    uint32_t loaded_fonts_generation_ = 0;
    AssetStore respak_store_;
    AssetStore tone_store_;
    AssetStore emoji_store_;
    AssetStore wake_word_store_;

    AssetStore& SelectStore(AssetStore& store);
    bool LoadEmoji();
    bool LoadWakeWord(bool force_reload);
};

#endif
