#pragma once

#include <cstdint>
#include <string>

enum class PlaybackSourceType {
    kUrl,
    kRomfs,
    kFile,
};

enum class PlaybackChannel {
    kPrompt,
    kMusic,
    kTts,
    kAlarm,
};

struct PlaybackRequest {
    uint32_t session_id = 0;
    PlaybackSourceType source_type = PlaybackSourceType::kUrl;
    PlaybackChannel channel = PlaybackChannel::kPrompt;
    std::string source;
    int priority = 0;
    bool resumable = false;
    bool loop = false;
};
