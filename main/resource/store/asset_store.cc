#include "asset_store.h"

#include <romfs.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <spi_flash_mmap.h>

#include <cstring>

#define TAG "AssetStore"

AssetStore::AssetStore() {
    Mount();
}

AssetStore::~AssetStore() {
    Unmount();
}

uint32_t AssetStore::CalculateChecksum(const char* data, uint32_t length) const {
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < length; ++i) {
        checksum += static_cast<unsigned char>(data[i]);
    }
    return checksum & 0xFFFF;
}

bool AssetStore::Mount() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return MountLocked();
}

bool AssetStore::MountLocked() {
    UnmountLocked();

    partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                           ESP_PARTITION_SUBTYPE_ANY,
                                           "assets");
    if (partition_ == nullptr) {
        ESP_LOGI(TAG, "No assets partition found");
        return false;
    }

    const int free_pages = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
    const uint32_t storage_size = static_cast<uint32_t>(free_pages) * 64 * 1024;
    ESP_LOGI(TAG, "The storage free size is %ld KB", storage_size / 1024);
    ESP_LOGI(TAG, "The partition size is %ld KB", partition_->size / 1024);
    if (storage_size < partition_->size) {
        ESP_LOGE(TAG, "The free size %ld KB is less than assets partition required %ld KB",
                 storage_size / 1024, partition_->size / 1024);
        return false;
    }

    const esp_err_t err = esp_partition_mmap(
        partition_, 0, partition_->size, ESP_PARTITION_MMAP_DATA,
        reinterpret_cast<const void**>(&mmap_root_), &mmap_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap assets partition: %s", esp_err_to_name(err));
        return false;
    }

    if (partition_->size >= 8 && std::memcmp(mmap_root_, "-rom1fs-", 8) == 0) {
        if (romfs_init(&romfs_, mmap_root_, partition_->size) != 0) {
            ESP_LOGE(TAG, "ROMFS 资源分区初始化失败");
            UnmountLocked();
            return false;
        }
        partition_valid_ = true;
        checksum_valid_ = true;
        ++generation_;
        ESP_LOGI(TAG, "已使用 ROMFS 资源包");
        return true;
    }

    partition_valid_ = true;
    if (partition_->size < 12) {
        ESP_LOGE(TAG, "Legacy assets partition is too small");
        UnmountLocked();
        return false;
    }

    const uint32_t stored_files = *reinterpret_cast<const uint32_t*>(mmap_root_ + 0);
    const uint32_t stored_chksum = *reinterpret_cast<const uint32_t*>(mmap_root_ + 4);
    const uint32_t stored_len = *reinterpret_cast<const uint32_t*>(mmap_root_ + 8);
    if (stored_len > partition_->size - 12) {
        ESP_LOGD(TAG, "The stored_len (0x%lx) is greater than the partition size (0x%lx) - 12",
                 stored_len, partition_->size);
        UnmountLocked();
        return false;
    }

    const int64_t start_time = esp_timer_get_time();
    const uint32_t calculated_checksum = CalculateChecksum(mmap_root_ + 12, stored_len);
    const int64_t end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "The checksum calculation time is %d ms",
             static_cast<int>((end_time - start_time) / 1000));

    if (calculated_checksum != stored_chksum) {
        ESP_LOGE(TAG, "The calculated checksum (0x%lx) does not match the stored checksum (0x%lx)",
                 calculated_checksum, stored_chksum);
        UnmountLocked();
        return false;
    }

    checksum_valid_ = true;
    if (stored_files > stored_len / sizeof(mmap_assets_table)) {
        ESP_LOGE(TAG, "Legacy assets table is outside the stored payload");
        UnmountLocked();
        return false;
    }
    const size_t table_size = sizeof(mmap_assets_table) * stored_files;
    for (uint32_t i = 0; i < stored_files; ++i) {
        const auto* item = reinterpret_cast<const mmap_assets_table*>(
            mmap_root_ + 12 + i * sizeof(mmap_assets_table));
        assets_[item->asset_name] = LegacyAsset{
            .size = static_cast<size_t>(item->asset_size),
            .offset = static_cast<size_t>(12 + table_size + item->asset_offset),
        };
    }
    ++generation_;
    return true;
}

void AssetStore::Unmount() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    UnmountLocked();
}

void AssetStore::UnmountLocked() {
    if (romfs_ != nullptr) {
        romfs_deinit(&romfs_);
    }
    if (mmap_handle_ != 0) {
        esp_partition_munmap(mmap_handle_);
    }
    mmap_handle_ = 0;
    mmap_root_ = nullptr;
    partition_valid_ = false;
    checksum_valid_ = false;
    assets_.clear();
}

bool AssetStore::Get(std::string_view name, AssetView& view) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    view = {};
    if (name.empty() || mmap_root_ == nullptr) {
        return false;
    }

    if (romfs_ != nullptr) {
        std::string path(name);
        if (path.front() != '/') {
            path.insert(path.begin(), '/');
        }
        uint8_t* data = nullptr;
        uint32_t size = 0;
        if (romfs_info_get(romfs_, path.c_str(), &data, &size) != 0) {
            return false;
        }
        view = AssetView{.data = data, .size = size, .generation = generation_};
        return view.valid();
    }

    const auto it = assets_.find(std::string(name));
    if (it == assets_.end()) {
        return false;
    }
    const auto& asset = it->second;
    if (asset.offset + asset.size + 2 > partition_->size) {
        return false;
    }
    const auto* data = reinterpret_cast<const uint8_t*>(mmap_root_ + asset.offset);
    if (data[0] != 'Z' || data[1] != 'Z') {
        ESP_LOGE(TAG, "The asset %.*s is not valid with magic %02x%02x",
                 static_cast<int>(name.size()), name.data(), data[0], data[1]);
        return false;
    }
    view = AssetView{.data = data + 2, .size = asset.size, .generation = generation_};
    return view.valid();
}

bool AssetStore::GetAssetData(const std::string& name, void*& ptr, size_t& size) const {
    AssetView view;
    if (!Get(name, view)) {
        ptr = nullptr;
        size = 0;
        return false;
    }
    ptr = const_cast<uint8_t*>(view.data);
    size = view.size;
    return true;
}

bool AssetStore::List(std::string_view directory, std::vector<std::string>& paths) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    paths.clear();
    if (romfs_ != nullptr) {
        romfs_dir_iter iter{};
        const std::string dir(directory);
        if (romfs_dir_iter_start(romfs_, dir.c_str(), &iter) != 0) {
            return false;
        }
        char path[ROMFS_PATH_MAX] = {};
        uint8_t* data = nullptr;
        uint32_t size = 0;
        bool is_dir = false;
        while (romfs_dir_iter_next(&iter, path, sizeof(path), &data, &size, &is_dir) == 0) {
            if (!is_dir && data != nullptr && size > 0) {
                paths.emplace_back(path);
            }
        }
        return true;
    }

    std::string prefix(directory);
    if (!prefix.empty() && prefix.front() == '/') {
        prefix.erase(prefix.begin());
    }
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }
    for (const auto& item : assets_) {
        if (item.first.rfind(prefix, 0) == 0) {
            paths.emplace_back("/" + item.first);
        }
    }
    return !paths.empty();
}

bool AssetStore::ListAssetFiles(const std::string& directory,
                                std::vector<std::string>& paths) const {
    return List(directory, paths);
}

bool AssetStore::partition_valid() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return partition_valid_;
}

bool AssetStore::checksum_valid() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return checksum_valid_;
}

uint32_t AssetStore::generation() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return generation_;
}
