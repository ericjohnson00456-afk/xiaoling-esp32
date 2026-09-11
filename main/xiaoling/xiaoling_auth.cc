#include "xiaoling_auth.h"

#include "assets/lang_config.h"
#include "board.h"
#include "cloud/cloud_http.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <mbedtls/md5.h>

#include <cctype>
#include <cstdio>
#include <ctime>

#define TAG "XiaolingAuth"

namespace {
std::string Trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool IsSystemTimeValid() {
    const auto now = std::time(nullptr);
    std::tm utc_time = {};
    return gmtime_r(&now, &utc_time) != nullptr && utc_time.tm_year >= (2024 - 1900);
}

bool EnsureSystemTime() {
    if (IsSystemTimeValid()) {
        return true;
    }

    // OTA 接口通常已经校准时间；这里为直接连接小聆的场景提供兜底。
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    const auto init_result = esp_netif_sntp_init(&config);
    const bool owns_sntp = init_result == ESP_OK;
    if (init_result != ESP_OK && init_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SNTP initialization failed: %s", esp_err_to_name(init_result));
        return false;
    }

    const auto sync_result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    if (owns_sntp) {
        esp_netif_sntp_deinit();
    }
    if (sync_result != ESP_OK || !IsSystemTimeValid()) {
        ESP_LOGE(TAG, "SNTP synchronization failed: %s", esp_err_to_name(sync_result));
        return false;
    }
    return true;
}

std::string CalculateChecksum(const std::string& input) {
    unsigned char digest[16] = {};
    if (mbedtls_md5(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest) != 0) {
        return {};
    }

    char checksum[33] = {};
    for (size_t i = 0; i < sizeof(digest); ++i) {
        std::snprintf(checksum + i * 2, 3, "%02x", digest[i]);
    }
    return checksum;
}
} // namespace

bool XiaolingAuthConfig::IsValid() const {
    return !access_token.empty();
}

XiaolingAuth& XiaolingAuth::GetInstance() {
    static XiaolingAuth instance;
    return instance;
}

XiaolingAuthConfig XiaolingAuth::Load() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool XiaolingAuth::Save(const XiaolingAuthConfig& config) const {
    if (!config.IsValid()) {
        ESP_LOGE(TAG, "Invalid Xiaoling authentication configuration");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    ESP_LOGI(TAG, "lschat access token updated, bytes=%u",
             static_cast<unsigned>(config_.access_token.size()));
    return true;
}

void XiaolingAuth::Clear() const {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = {};
}

bool XiaolingAuth::Authenticate(const std::string& url,
                                const std::string& product_id,
                                const std::string& device_id,
                                const std::string& secret_id,
                                const std::string& extra_code) const {
    if (url.empty() || product_id.empty() || device_id.empty() || secret_id.empty()) {
        ESP_LOGE(TAG, "lschat authentication parameters are incomplete");
        return false;
    }
    if (!EnsureSystemTime()) {
        return false;
    }

    const auto current_time = std::time(nullptr);
    const auto current_time_text = std::to_string(static_cast<long long>(current_time));
    const auto checksum = CalculateChecksum(secret_id + device_id + current_time_text);
    if (checksum.empty()) {
        ESP_LOGE(TAG, "Failed to calculate lschat authentication checksum");
        return false;
    }

    cJSON* request = cJSON_CreateObject();
    if (request == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(request, "productId", product_id.c_str());
    cJSON_AddStringToObject(request, "deviceId", device_id.c_str());
    cJSON_AddNumberToObject(request, "curtime", static_cast<double>(current_time));
    cJSON_AddStringToObject(request, "checksum", checksum.c_str());
    if (!extra_code.empty()) {
        cJSON_AddStringToObject(request, "code", extra_code.c_str());
    }

    char* request_text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (request_text == nullptr) {
        return false;
    }
    const std::string request_body(request_text);
    cJSON_free(request_text);

    // 这些请求头属于小聆鉴权协议，不下沉到通用 HTTP 传输层。
    const CloudHttpHeaders headers = {
        {"Activation-Version", "1"},
        {"Device-Id", device_id},
        {"Client-Id", Board::GetInstance().GetUuid()},
        {"User-Agent", SystemInfo::GetUserAgent()},
        {"Accept-Language", Lang::CODE},
    };
    CloudHttpResponse response;
    CloudHttp http;
    if (!http.PostJson(url, request_body, response, headers) ||
        response.status_code < 200 || response.status_code >= 300) {
        ESP_LOGE(TAG, "lschat authentication failed, status=%d", response.status_code);
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(response.body.data(), response.body.size());
    if (root == nullptr) {
        ESP_LOGE(TAG, "lschat authentication returned invalid JSON");
        return false;
    }
    const auto* token = cJSON_GetObjectItemCaseSensitive(root, "token");
    XiaolingAuthConfig auth_config;
    if (cJSON_IsString(token) && token->valuestring != nullptr) {
        auth_config.access_token = token->valuestring;
    }
    cJSON_Delete(root);

    if (!Save(auth_config)) {
        ESP_LOGE(TAG, "lschat authentication response has no token");
        return false;
    }
    return true;
}

std::string XiaolingAuth::NormalizeBearerToken(const std::string& token) {
    auto normalized = Trim(token);
    if (normalized.empty()) {
        return normalized;
    }

    constexpr const char* kBearer = "Bearer ";
    constexpr size_t kBearerLength = 7;
    bool has_bearer = normalized.size() >= kBearerLength;
    if (has_bearer) {
        for (size_t i = 0; i < kBearerLength; ++i) {
            if (std::tolower(static_cast<unsigned char>(normalized[i])) !=
                std::tolower(static_cast<unsigned char>(kBearer[i]))) {
                has_bearer = false;
                break;
            }
        }
    }
    return has_bearer ? normalized : std::string(kBearer) + normalized;
}
