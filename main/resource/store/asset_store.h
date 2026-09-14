#pragma once

#include <esp_partition.h>

#include <cstdint>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct romfs;

struct AssetView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t generation = 0;

    bool valid() const { return data != nullptr && size != 0; }
};

// Shared, storage-only resource access. It knows how to mount the assets
// partition and read both the current ROMFS format and the legacy indexed
// format, but it does not know anything about UI, audio, or model loading.
class AssetStore {
public:
    AssetStore();
    virtual ~AssetStore();

    AssetStore(const AssetStore&) = delete;
    AssetStore& operator=(const AssetStore&) = delete;

    bool Mount();
    void Unmount();

    // The returned memory is read-only and remains valid until Mount/Unmount
    // is called. Consumers that retain a view should record generation().
    bool Get(std::string_view name, AssetView& view) const;
    bool List(std::string_view directory, std::vector<std::string>& paths) const;

    // Compatibility form for consumers that have not migrated to AssetView.
    bool GetAssetData(const std::string& name, void*& ptr, size_t& size) const;
    bool ListAssetFiles(const std::string& directory, std::vector<std::string>& paths) const;

    bool partition_valid() const;
    bool checksum_valid() const;
    uint32_t generation() const;

protected:
    const esp_partition_t* partition() const { return partition_; }

private:
    struct LegacyAsset {
        size_t size;
        size_t offset;
    };

    struct mmap_assets_table {
        char asset_name[32];
        uint32_t asset_size;
        uint32_t asset_offset;
        uint16_t asset_width;
        uint16_t asset_height;
    };

    bool MountLocked();
    void UnmountLocked();
    uint32_t CalculateChecksum(const char* data, uint32_t length) const;

    mutable std::recursive_mutex mutex_;
    const esp_partition_t* partition_ = nullptr;
    esp_partition_mmap_handle_t mmap_handle_ = 0;
    const char* mmap_root_ = nullptr;
    bool partition_valid_ = false;
    bool checksum_valid_ = false;
    uint32_t generation_ = 0;
    std::map<std::string, LegacyAsset> assets_;
    romfs* romfs_ = nullptr;
};
