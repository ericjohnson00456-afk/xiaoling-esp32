#include "resource.h"

#include <romfs.h>

#include <esp_log.h>

#include <array>
#include <cctype>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr char kTag[] = "Resource";
constexpr size_t kPackageCount = 4;

struct PartitionState {
    const char* label;
    const esp_partition_t* partition = nullptr;
    esp_partition_mmap_handle_t mmap_handle = 0;
    romfs* fs = nullptr;
};

// 资源模块只有四份分区状态和一个 Tone ID 小表，不保存普通资源路径表。
std::array<PartitionState, kPackageCount> partitions{{
    {.label = "respak"},
    {.label = "tone"},
    {.label = "emoji"},
    {.label = "wake_word"},
}};
std::array<ResourceData, Resource::kToneCount> tones{};
std::mutex mutex;

PartitionState& Partition(ResourcePackage package) {
    return partitions[static_cast<size_t>(package)];
}

std::string NormalizePath(std::string_view path) {
    std::string normalized(path);
    if (normalized.empty() || normalized.front() != '/') {
        normalized.insert(normalized.begin(), '/');
    }
    return normalized;
}

void UnmountPartition(PartitionState& state) {
    // 必须先释放 ROMFS 解析状态，再取消它所引用的 Flash 地址映射。
    if (state.fs != nullptr) {
        romfs_deinit(&state.fs);
    }
    if (state.mmap_handle != 0) {
        esp_partition_munmap(state.mmap_handle);
        state.mmap_handle = 0;
    }
}

bool MountPartition(PartitionState& state) {
    UnmountPartition(state);
    if (state.partition == nullptr) {
        state.partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, state.label);
    }
    if (state.partition == nullptr) {
        ESP_LOGE(kTag, "Resource partition not found: %s", state.label);
        return false;
    }

    // 将整个分区映射到数据地址空间，ROMFS 返回的文件地址都指向这块映射。
    const void* address = nullptr;
    const esp_err_t err = esp_partition_mmap(
        state.partition, 0, state.partition->size, ESP_PARTITION_MMAP_DATA,
        &address, &state.mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mmap %s: %s", state.label,
                 esp_err_to_name(err));
        return false;
    }
    if (romfs_init(&state.fs, address, state.partition->size) != 0) {
        ESP_LOGE(kTag, "Failed to initialize ROMFS: %s", state.label);
        esp_partition_munmap(state.mmap_handle);
        state.mmap_handle = 0;
        return false;
    }
    return true;
}

using FileVisitor = bool (*)(const char* path, const uint8_t* address,
                             size_t size, void* context);

// ROMFS 迭代器一次只遍历一层目录，这里用目录栈完成递归遍历。
bool WalkFiles(romfs* fs, std::string_view root, FileVisitor visitor,
               void* context) {
    std::vector<std::string> directories{NormalizePath(root)};
    while (!directories.empty()) {
        std::string directory = std::move(directories.back());
        directories.pop_back();

        romfs_dir_iter iterator{};
        if (romfs_dir_iter_start(fs, directory.c_str(), &iterator) != 0) {
            return false;
        }
        while (true) {
            char path[ROMFS_PATH_MAX] = {};
            uint8_t* address = nullptr;
            uint32_t size = 0;
            bool is_directory = false;
            const int result = romfs_dir_iter_next(
                &iterator, path, sizeof(path), &address, &size, &is_directory);
            // 返回 1 表示目录遍历结束，返回负数表示出错。
            if (result > 0) {
                break;
            }
            if (result < 0) {
                return false;
            }
            if (is_directory) {
                directories.emplace_back(path);
            } else if (address != nullptr &&
                       !visitor(path, address, size, context)) {
                return false;
            }
        }
    }
    return true;
}

bool ParseToneId(std::string_view path, uint16_t& id) {
    // Tone 文件名格式为 059_network_suc.mp3，前三位就是资源 ID。
    const size_t slash = path.find_last_of('/');
    const std::string_view filename = path.substr(
        slash == std::string_view::npos ? 0 : slash + 1);
    if (filename.size() < 5 || filename[3] != '_' ||
        !std::isdigit(static_cast<unsigned char>(filename[0])) ||
        !std::isdigit(static_cast<unsigned char>(filename[1])) ||
        !std::isdigit(static_cast<unsigned char>(filename[2]))) {
        return false;
    }
    id = static_cast<uint16_t>((filename[0] - '0') * 100 +
                               (filename[1] - '0') * 10 + filename[2] - '0');
    return id < Resource::kToneCount;
}

struct ToneIndexContext {
    size_t count = 0;
    bool valid = true;
};

bool AddTone(const char* path, const uint8_t* address, size_t size,
             void* opaque) {
    auto& context = *static_cast<ToneIndexContext*>(opaque);
    uint16_t id = 0;
    if (!ParseToneId(path, id)) {
        ESP_LOGW(kTag, "Ignoring tone without a valid ID: %s", path);
        return true;
    }
    if (tones[id].address != nullptr) {
        ESP_LOGE(kTag, "Duplicate tone ID %u: %s",
                 static_cast<unsigned>(id), path);
        context.valid = false;
        return true;
    }
    tones[id] = {.address = address, .size = size};
    ++context.count;
    return true;
}

bool BuildToneIndex() {
    // Tone 播放是高频 ID 查询，只为它保留固定大小的 address/size 数组。
    tones.fill({});
    ToneIndexContext context;
    const bool walked = WalkFiles(Partition(ResourcePackage::Tone).fs, "/",
                                  AddTone, &context);
    ESP_LOGI(kTag, "Indexed %u tone IDs",
             static_cast<unsigned>(context.count));
    return walked && context.valid && context.count != 0;
}

bool MountPackage(ResourcePackage package) {
    if (!MountPartition(Partition(package))) {
        return false;
    }
    return package != ResourcePackage::Tone || BuildToneIndex();
}

bool CollectPath(const char* path, const uint8_t*, size_t, void* context) {
    static_cast<std::vector<std::string>*>(context)->emplace_back(path);
    return true;
}

}  // namespace

namespace Resource {

bool Initialize() {
    std::lock_guard<std::mutex> lock(mutex);
    bool ready = true;
    for (size_t i = 0; i < kPackageCount; ++i) {
        ready = MountPackage(static_cast<ResourcePackage>(i)) && ready;
    }
    if (!ready) {
        ESP_LOGE(kTag, "One or more resource packages failed to initialize");
    }
    return ready;
}

bool Get(ResourcePackage package, std::string_view path, ResourceData& resource) {
    std::lock_guard<std::mutex> lock(mutex);
    resource = {};
    auto& partition = Partition(package);
    if (partition.fs == nullptr) {
        return false;
    }

    // 普通资源不经过第二份索引表，直接让 ROMFS 根据路径返回地址和大小。
    const std::string normalized = NormalizePath(path);
    uint8_t* address = nullptr;
    uint32_t size = 0;
    if (romfs_info_get(partition.fs, normalized.c_str(), &address, &size) != 0) {
        return false;
    }
    resource = {.address = address, .size = size};
    return true;
}

bool GetTone(uint16_t id, ResourceData& resource) {
    std::lock_guard<std::mutex> lock(mutex);
    resource = {};
    if (id >= tones.size() || tones[id].address == nullptr) {
        return false;
    }
    resource = tones[id];
    return true;
}

bool List(ResourcePackage package, std::string_view directory,
          std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lock(mutex);
    paths.clear();
    auto& partition = Partition(package);
    // 仅在调用者需要发现文件时临时遍历，不长期保存路径字符串。
    return partition.fs != nullptr &&
           WalkFiles(partition.fs, directory, CollectPath, &paths) &&
           !paths.empty();
}

bool IsAvailable(ResourcePackage package) {
    std::lock_guard<std::mutex> lock(mutex);
    return Partition(package).fs != nullptr;
}

const esp_partition_t* GetPartition(ResourcePackage package) {
    std::lock_guard<std::mutex> lock(mutex);
    auto& state = Partition(package);
    if (state.partition == nullptr) {
        state.partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, state.label);
    }
    return state.partition;
}

void Unload(ResourcePackage package) {
    std::lock_guard<std::mutex> lock(mutex);
    if (package == ResourcePackage::Tone) {
        tones.fill({});
    }
    UnmountPartition(Partition(package));
}

bool Reload(ResourcePackage package) {
    std::lock_guard<std::mutex> lock(mutex);
    if (package == ResourcePackage::Tone) {
        tones.fill({});
    }
    return MountPackage(package);
}

}  // namespace Resource
