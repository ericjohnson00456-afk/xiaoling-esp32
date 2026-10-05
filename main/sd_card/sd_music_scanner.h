#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <esp_err.h>

#include "sd_music_types.h"

class SdMusicScanner {
public:
    static constexpr size_t kBatchSize = 100;
    static constexpr size_t kLocalListLimit = 100;

    using BatchCallback = std::function<esp_err_t(
        const std::vector<SdMusicTrack>& tracks, bool is_last)>;

    // 默认只扫描 /sdcard/audio，不扫描该目录之外的文件。
    // max_total 为 0 表示不限制，目录层级最多为扫描目录下三层。
    static esp_err_t Scan(const std::string& path, size_t max_total,
                          bool read_metadata, BatchCallback callback,
                          SdMusicScanStats* stats = nullptr,
                          const std::atomic_bool* cancelled = nullptr);
};
