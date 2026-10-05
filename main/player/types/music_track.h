#pragma once

#include <cstdint>
#include <string>

// 播放器使用的曲目元数据。来源字段为后续接入 TF 卡音乐预留。
enum class MusicSource : uint8_t {
    kUnknown,
    kKuwoOnline,
    kTfCard,
};

struct MusicTrack {
    std::string id;
    // 在线音乐保存待解析 ID；TF 卡音乐在这里保存可直接播放的完整路径。
    std::string location;
    std::string name;
    MusicSource source = MusicSource::kUnknown;
};
