#pragma once

#include <esp_partition.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class ResourcePackage : uint8_t {
    Respak,
    Tone,
    Emoji,
    WakeWord,
};

// 资源内容直接映射自 Flash，不会复制到 RAM。
// 对应资源包执行 Unload() 或 Reload() 后，原 address 立即失效。
struct ResourceData {
    const uint8_t* address = nullptr;
    size_t size = 0;
};

namespace Resource {

inline constexpr uint16_t kToneCount = 300;

// 挂载全部 ROMFS 资源分区。普通资源按需查询，启动时只建立 Tone ID 小表。
bool Initialize();

// 根据资源包和 ROMFS 路径查询 Flash 地址与大小。
bool Get(ResourcePackage package, std::string_view path, ResourceData& resource);

// Tone 文件名以三位数字 ID 开头，通过启动时建立的小表直接查询。
bool GetTone(uint16_t id, ResourceData& resource);

// 按需递归列出目录中的文件。字体、唤醒词模型等需要发现文件时使用，
// 返回的路径可以直接传给 Get()。
bool List(ResourcePackage package, std::string_view directory,
          std::vector<std::string>& paths);

bool IsAvailable(ResourcePackage package);
const esp_partition_t* GetPartition(ResourcePackage package);

// OTA 卸载资源包前必须先停止所有使用者，因为已有 address 会立即失效。
void Unload(ResourcePackage package);
bool Reload(ResourcePackage package);

}  // namespace Resource
