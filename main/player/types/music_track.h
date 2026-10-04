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
    std::string name;
    MusicSource source = MusicSource::kUnknown;
};
