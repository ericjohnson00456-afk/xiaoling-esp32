#include "xiaoling_conn.h"

#include "ui/i18n/lang_config.h"
#include "board.h"
#include "cloud/cloud_http.h"
#include "system/system_info.h"
#include "xiaoling_config.h"

#include <cJSON.h>
#include <esp_log.h>
#include <mbedtls/md5.h>

#include <cctype>
#include <cstdio>
#include <ctime>

#define TAG "XiaolingConn"

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

std::string CalculateChecksum(const std::string& input) {
    unsigned char digest[16] = {};
    if (mbedtls_md5(reinterpret_cast<const unsigned char*>(input.data()),
                    input.size(), digest) != 0) {
        return {};
    }

    char checksum[33] = {};
    for (size_t i = 0; i < sizeof(digest); ++i) {
        std::snprintf(checksum + i * 2, 3, "%02x", digest[i]);
    }
    return checksum;
}

}  // 匿名命名空间

bool XiaolingConn::EnsureAuthenticated() {
    const auto cached_token = LoadAccessToken();
    if (!cached_token.empty()) {
        return true;
    }
    if (SystemInfo::GetSecretId().empty()) {
        ESP_LOGE(TAG, "CONFIG_LSCHAT_SECRET_ID 为空");
        SetError(XiaolingError::ServerNotConnected);
        return false;
    }
    return Authenticate();
}

bool XiaolingConn::Authenticate(const std::string& extra_code) {
    const std::string url = XiaolingConfig::kTokenUrl;
    const auto product_id = SystemInfo::GetProductId();
    const auto device_id = SystemInfo::GetCloudDeviceId();
    const auto secret_id = SystemInfo::GetSecretId();
    if (url.empty() || product_id.empty() || device_id.empty() || secret_id.empty()) {
        ESP_LOGE(TAG, "小聆鉴权参数不完整");
        SetError(XiaolingError::ServerNotConnected);
        return false;
    }
    const auto current_time = std::time(nullptr);
    const auto current_time_text = std::to_string(static_cast<long long>(current_time));
    const auto checksum = CalculateChecksum(secret_id + device_id + current_time_text);
    if (checksum.empty()) {
        ESP_LOGE(TAG, "计算小聆鉴权校验值失败");
        SetError(XiaolingError::ServerNotConnected);
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
        ESP_LOGE(TAG, "小聆鉴权失败: status=%d", response.status_code);
        SetError(XiaolingError::ServerNotConnected);
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(response.body.data(), response.body.size());
    if (root == nullptr) {
        ESP_LOGE(TAG, "小聆鉴权响应不是有效 JSON");
        return false;
    }
    const auto* token = cJSON_GetObjectItemCaseSensitive(root, "token");
    std::string access_token;
    if (cJSON_IsString(token) && token->valuestring != nullptr) {
        access_token = token->valuestring;
    }
    cJSON_Delete(root);
    if (access_token.empty()) {
        ESP_LOGE(TAG, "小聆鉴权响应中没有 Token");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        access_token_ = std::move(access_token);
    }
    return true;
}

void XiaolingConn::ClearAuthentication() {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    access_token_.clear();
}

std::string XiaolingConn::LoadAccessToken() const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    return access_token_;
}

std::string XiaolingConn::NormalizeBearerToken(const std::string& token) {
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
