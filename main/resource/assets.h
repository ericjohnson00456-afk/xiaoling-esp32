#ifndef ASSETS_H
#define ASSETS_H

#include "resource/store/asset_store.h"

#include <string>
#include <functional>
#include <array>

#include <cJSON.h>
#include <model_path.h>

// Product-specific resource package management. The base store is respak
// (falling back to the legacy assets partition); tone, emoji and wake_word are
// independently mounted and downloadable when their partitions exist.
class Assets : public AssetStore {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }
    ~Assets() override = default;

    bool Download(std::string url, std::function<void(int progress, size_t speed)> progress_callback);
    enum class Resource { Respak, Tone, Emoji, WakeWord };
    // Download one independently managed resource partition. The URL is
    // expected to contain a complete ROMFS image for that resource.
    bool DownloadResource(Resource resource, std::string url,
                          std::function<void(int progress, size_t speed)> progress_callback);
    bool DownloadResource(Resource resource, const std::string& url, size_t expected_size,
                          const std::array<uint8_t, 16>& expected_md5,
                          std::function<void(int progress, size_t speed)> progress_callback);
    bool VerifyResource(Resource resource, size_t expected_size,
                        const std::array<uint8_t, 16>& expected_md5);
    bool Apply();
    AssetStore& RespakStore() { return *this; }
    AssetStore& ToneStore();
    AssetStore& EmojiStore();
    AssetStore& WakeWordStore();
    using AssetStore::GetAssetData;
    // 列出资源目录下的文件路径，ROMFS 会保留完整层级。
    using AssetStore::ListAssetFiles;

    inline std::string default_assets_url() const { return default_assets_url_; }

private:
    Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    std::string default_assets_url_;
    srmodel_list_t* models_list_ = nullptr;
    AssetStore tone_store_;
    AssetStore emoji_store_;
    AssetStore wake_word_store_;

    AssetStore& SelectStore(Resource resource);
};

#endif
