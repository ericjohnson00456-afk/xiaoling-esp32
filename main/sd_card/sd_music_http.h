#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <esp_err.h>

#include "sd_music_types.h"

struct SdMusicHttpResult {
    esp_err_t error = ESP_OK;
    int status_code = 0;
};

class SdMusicHttp {
public:
    SdMusicHttp(std::string authorization, std::string host);

    SdMusicHttpResult ReportCardEvent(const std::string& card_id,
                                      const char* event_type,
                                      uint64_t capacity_bytes) const;
    SdMusicHttpResult ReportFiles(const std::string& card_id,
                                  const std::string& scan_id,
                                  const std::vector<SdMusicTrack>& tracks,
                                  bool is_last_batch) const;

private:
    SdMusicHttpResult PostJson(const char* path, const std::string& body) const;
    SdMusicHttpResult PostFiles(const std::string& card_id,
                                const std::string& scan_id,
                                const std::vector<SdMusicTrack>& tracks,
                                bool is_last_batch) const;

    std::string authorization_;
    std::string host_;
};
