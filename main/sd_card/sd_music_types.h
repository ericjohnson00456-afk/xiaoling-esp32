#pragma once

#include <cstdint>
#include <string>

struct SdMusicTrack {
    std::string path;
    std::string file_name;
    std::string title;
    std::string artist;
    std::string album;
};

struct SdMusicScanStats {
    uint32_t reported_count = 0;
    uint32_t skipped_count = 0;
    uint32_t unknown_skip_count = 0;
};

enum class SdMusicSyncPhase : uint8_t {
    kIdle,
    kScanning,
    kUploading,
    kSucceeded,
    kFailed,
};

enum class SdMusicSyncResult : int32_t {
    kOk = 0,
    kDataUnchanged = 1,
    kNoMp3 = 2,
    kUnsupportedFileSystem = 3,
    kFailed = -1,
};

struct SdMusicSyncState {
    SdMusicSyncPhase phase = SdMusicSyncPhase::kIdle;
    uint32_t uploaded_count = 0;
    uint32_t total_count = 0;
    SdMusicSyncResult result = SdMusicSyncResult::kOk;
    uint32_t http_status_code = 0;
    int32_t http_error_code = 0;
    uint32_t skipped_count = 0;
};
