#ifndef ASSETS_H
#define ASSETS_H

#include "resource/store/asset_store.h"

#include <string>
#include <functional>

#include <cJSON.h>
#include <model_path.h>

// Product-specific resource package management. Low-level partition access is
// provided by the shared AssetStore base class; this class retains download,
// apply, and legacy model integration until those consumers migrate.
class Assets : public AssetStore {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }
    ~Assets() override = default;

    bool Download(std::string url, std::function<void(int progress, size_t speed)> progress_callback);
    bool Apply();
    using AssetStore::GetAssetData;
    // 列出资源目录下的文件路径，ROMFS 会保留完整层级。
    using AssetStore::ListAssetFiles;

    inline std::string default_assets_url() const { return default_assets_url_; }

private:
    Assets() = default;
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    std::string default_assets_url_;
    srmodel_list_t* models_list_ = nullptr;
};

#endif
