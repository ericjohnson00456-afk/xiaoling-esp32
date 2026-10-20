#include "sd_music_http.h"

#include "board.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr char kTag[] = "SdMusicHttp";
constexpr char kCardEventPath[] = "/v1/media/tf-card/events";
constexpr char kFilesImportPath[] = "/v1/media/tf-card/files/import";
constexpr int kTimeoutMs = 10000;
constexpr int kFileReportRetries = 3;
constexpr uint32_t kRetryDelayBaseMs = 300;
constexpr size_t kRequestLogPreviewBytes = 4096;

bool MatchUtf8Sequence(const uint8_t* data, size_t remaining, size_t& length) {
    length = 0;
    if (remaining == 0) return false;
    const uint8_t first = data[0];
    if (first <= 0x7f) {
        length = 1;
        return true;
    }
    if (first >= 0xc2 && first <= 0xdf && remaining >= 2 &&
        (data[1] & 0xc0) == 0x80) {
        length = 2;
        return true;
    }
    if (remaining >= 3 &&
        ((first == 0xe0 && data[1] >= 0xa0 && data[1] <= 0xbf) ||
         ((first >= 0xe1 && first <= 0xec) && (data[1] & 0xc0) == 0x80) ||
         (first == 0xed && data[1] >= 0x80 && data[1] <= 0x9f) ||
         ((first >= 0xee && first <= 0xef) && (data[1] & 0xc0) == 0x80)) &&
        (data[2] & 0xc0) == 0x80) {
        length = 3;
        return true;
    }
    if (remaining >= 4 &&
        ((first == 0xf0 && data[1] >= 0x90 && data[1] <= 0xbf) ||
         ((first >= 0xf1 && first <= 0xf3) && (data[1] & 0xc0) == 0x80) ||
         (first == 0xf4 && data[1] >= 0x80 && data[1] <= 0x8f)) &&
        (data[2] & 0xc0) == 0x80 && (data[3] & 0xc0) == 0x80) {
        length = 4;
        return true;
    }
    return false;
}

void AppendJsonString(std::string& output, const std::string& input) {
    output.push_back('"');
    const auto* bytes = reinterpret_cast<const uint8_t*>(input.data());
    size_t offset = 0;
    while (offset < input.size()) {
        const uint8_t value = bytes[offset];
        if (value < 0x20) {
            char escaped[7] = {};
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", value);
            output += escaped;
            ++offset;
            continue;
        }
        if (value == '"' || value == '\\') {
            output.push_back('\\');
            output.push_back(static_cast<char>(value));
            ++offset;
            continue;
        }
        size_t sequence_length = 0;
        if (!MatchUtf8Sequence(bytes + offset, input.size() - offset,
                               sequence_length)) {
            ++offset;
            continue;
        }
        output.append(input, offset, sequence_length);
        offset += sequence_length;
    }
    output.push_back('"');
}

void AppendProperty(std::string& output, const char* name,
                    const std::string& value, bool& first) {
    if (!first) output.push_back(',');
    first = false;
    AppendJsonString(output, name);
    output.push_back(':');
    AppendJsonString(output, value);
}

std::string BuildFileJson(const SdMusicTrack& track) {
    std::string json;
    json.reserve(256 + track.path.size() + track.file_name.size() +
                 track.title.size() + track.artist.size() + track.album.size());
    json.push_back('{');
    bool first = true;
    AppendProperty(json, "file_path", track.path, first);
    AppendProperty(json, "file_name", track.file_name, first);
    if (!track.title.empty() || !track.artist.empty() || !track.album.empty()) {
        json += ",\"media_tags\":{";
        bool first_tag = true;
        if (!track.title.empty()) AppendProperty(json, "title", track.title, first_tag);
        if (!track.artist.empty()) AppendProperty(json, "artist", track.artist, first_tag);
        if (!track.album.empty()) AppendProperty(json, "album", track.album, first_tag);
        json.push_back('}');
    }
    json.push_back('}');
    return json;
}

bool WriteChunk(Http& http, const std::string& chunk) {
    return chunk.empty() || http.Write(chunk.data(), chunk.size()) > 0;
}

void AppendRequestLogPreview(std::string& preview, const std::string& chunk,
                             bool& truncated) {
    if (preview.size() >= kRequestLogPreviewBytes) {
        truncated = true;
        return;
    }
    const size_t remaining = kRequestLogPreviewBytes - preview.size();
    preview.append(chunk, 0, std::min(remaining, chunk.size()));
    if (chunk.size() > remaining) truncated = true;
}

}  // namespace

SdMusicHttp::SdMusicHttp(std::string authorization, std::string host)
    : authorization_(std::move(authorization)), host_(std::move(host)) {}

SdMusicHttpResult SdMusicHttp::PostJson(const char* path,
                                        const std::string& body) const {
    if (authorization_.empty() || host_.empty()) {
        return {ESP_ERR_INVALID_STATE, 0};
    }
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return {ESP_ERR_INVALID_STATE, 0};
    auto http = network->CreateHttp(0);
    if (http == nullptr) return {ESP_ERR_NO_MEM, 0};
    http->SetTimeout(kTimeoutMs);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Authorization", authorization_);
    http->SetContent(std::string(body));
    const std::string url = std::string("https://") + host_ + path;
    if (!http->Open("POST", url)) {
        http->Close();
        return {ESP_FAIL, 0};
    }
    const int status = http->GetStatusCode();
    const std::string response = http->ReadAll();
    http->Close();
    const size_t preview = std::min<size_t>(response.size(), 256);
    ESP_LOGI(kTag, "POST %s: status=%d, response=%.*s", path, status,
             static_cast<int>(preview), response.data());
    return {status == 200 ? ESP_OK : ESP_FAIL, status};
}

SdMusicHttpResult SdMusicHttp::ReportCardEvent(
        const std::string& card_id, const char* event_type,
        uint64_t capacity_bytes) const {
    std::string body = "{\"event_type\":";
    AppendJsonString(body, event_type != nullptr ? event_type : "");
    body += ",\"card_id\":";
    AppendJsonString(body, card_id);
    if (event_type != nullptr && std::strcmp(event_type, "inserted") == 0) {
        body += ",\"card_capacity\":" + std::to_string(capacity_bytes);
    }
    body.push_back('}');
    ESP_LOGI(kTag, "上报 TF 卡事件: type=%s, card_id=%s",
             event_type, card_id.c_str());
    return PostJson(kCardEventPath, body);
}

SdMusicHttpResult SdMusicHttp::ReportFiles(
        const std::string& card_id, const std::string& scan_id,
        const std::vector<SdMusicTrack>& tracks, bool is_last_batch) const {
    if (tracks.empty() && !is_last_batch) return {};
    SdMusicHttpResult result;
    for (int attempt = 0; attempt <= kFileReportRetries; ++attempt) {
        ESP_LOGI(kTag,
                 "上报 TF 卡文件批次: count=%u, last=%d, attempt=%d/%d",
                 static_cast<unsigned>(tracks.size()), is_last_batch ? 1 : 0,
                 attempt + 1, kFileReportRetries + 1);
        result = PostFiles(card_id, scan_id, tracks, is_last_batch);
        if (result.error == ESP_OK) return result;
        if (attempt < kFileReportRetries) {
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayBaseMs * (attempt + 1)));
        }
    }
    ESP_LOGE(kTag, "TF 卡文件批次重试 %d 次后仍失败: status=%d",
             kFileReportRetries, result.status_code);
    return result;
}

SdMusicHttpResult SdMusicHttp::PostFiles(
        const std::string& card_id, const std::string& scan_id,
        const std::vector<SdMusicTrack>& tracks, bool is_last_batch) const {
    if (authorization_.empty() || host_.empty()) {
        return {ESP_ERR_INVALID_STATE, 0};
    }
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return {ESP_ERR_INVALID_STATE, 0};
    auto http = network->CreateHttp(0);
    if (http == nullptr) return {ESP_ERR_NO_MEM, 0};

    http->SetTimeout(kTimeoutMs);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Authorization", authorization_);
    http->SetHeader("Transfer-Encoding", "chunked");
    const std::string url = std::string("https://") + host_ + kFilesImportPath;
    if (!http->Open("POST", url)) {
        http->Close();
        return {ESP_FAIL, 0};
    }

    std::string prefix = "{\"card_id\":";
    AppendJsonString(prefix, card_id);
    prefix += ",\"scan_id\":";
    AppendJsonString(prefix, scan_id);
    prefix += ",\"is_last_batch\":";
    prefix += is_last_batch ? "true" : "false";
    prefix += ",\"files\":[";

    std::string request_log_preview;
    request_log_preview.reserve(kRequestLogPreviewBytes);
    bool request_log_truncated = false;
    AppendRequestLogPreview(request_log_preview, prefix, request_log_truncated);
    bool write_ok = WriteChunk(*http, prefix);
    for (size_t i = 0; write_ok && i < tracks.size(); ++i) {
        if (i != 0) {
            AppendRequestLogPreview(request_log_preview, ",", request_log_truncated);
            write_ok = WriteChunk(*http, ",");
        }
        if (write_ok) {
            const std::string file_json = BuildFileJson(tracks[i]);
            AppendRequestLogPreview(request_log_preview, file_json,
                                    request_log_truncated);
            write_ok = WriteChunk(*http, file_json);
        }
    }
    if (write_ok) {
        AppendRequestLogPreview(request_log_preview, "]}", request_log_truncated);
        write_ok = WriteChunk(*http, "]}");
    }
    if (write_ok) write_ok = http->Write("", 0) >= 0;
    if (!write_ok) {
        ESP_LOGW(kTag, "发送 TF 卡文件批次失败");
        http->Close();
        return {ESP_FAIL, 0};
    }

    ESP_LOGI(kTag, "TF 卡文件列表请求体%s: %s",
             request_log_truncated ? "(前4096字节)" : "",
             request_log_preview.c_str());

    const int status = http->GetStatusCode();
    const std::string response = http->ReadAll();
    http->Close();
    const size_t preview = std::min<size_t>(response.size(), 256);
    ESP_LOGI(kTag, "POST %s: status=%d, response=%.*s", kFilesImportPath,
             status, static_cast<int>(preview), response.data());
    return {status == 200 ? ESP_OK : ESP_FAIL, status};
}
