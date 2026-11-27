#include "camera_upload.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "board.h"
#include "system/network_compat.h"
#include "system/system_info.h"

namespace {

void WriteAll(Http& http, const char* data, size_t size) {
    while (size > 0) {
        const size_t chunk = std::min(size, size_t{4096});
        const int written = NetworkValue(http.Write(data, chunk));
        if (written <= 0 || static_cast<size_t>(written) > chunk) {
            throw std::runtime_error("Failed to upload photo body");
        }
        data += written;
        size -= written;
    }
}

struct HttpCloser {
    Http& http;
    ~HttpCloser() { http.Close(); }
};

}  // namespace

std::string UploadCameraJpeg(const std::string& url, const std::string& token,
                             const std::string& question,
                             const uint8_t* jpeg, size_t size) {
    if (url.empty() || jpeg == nullptr || size == 0) {
        throw std::runtime_error("Missing photo or upload URL");
    }
    const std::string boundary = "----ESP32_CAMERA_BOUNDARY";
    const std::string prefix = "--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n" + question +
        "\r\n--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\""
        "\r\nContent-Type: image/jpeg\r\n\r\n";
    const std::string footer = "\r\n--" + boundary + "--\r\n";
    if (size > std::numeric_limits<size_t>::max() - prefix.size() - footer.size()) {
        throw std::runtime_error("Photo upload is too large");
    }
    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();
    if (network == nullptr) {
        throw std::runtime_error("Network is unavailable");
    }
    auto http = network->CreateHttp(3);
    if (!http) {
        throw std::runtime_error("Failed to create photo HTTP client");
    }
    HttpCloser closer{*http};
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Client-Id", board.GetUuid());
    if (!token.empty()) {
        http->SetHeader("Authorization", "Bearer " + token);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Content-Length", std::to_string(prefix.size() + size + footer.size()));
    if (!http->Open("POST", url)) {
        throw std::runtime_error("Failed to connect to photo upload URL");
    }
    WriteAll(*http, prefix.data(), prefix.size());
    WriteAll(*http, reinterpret_cast<const char*>(jpeg), size);
    WriteAll(*http, footer.data(), footer.size());
    if (NetworkValue(http->GetStatusCode()) != 200) {
        throw std::runtime_error("Failed to upload photo");
    }
    return http->ReadAll();
}
