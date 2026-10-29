#include "xiaoling_conn.h"

#include "ui/i18n/lang_config.h"
#include "board.h"
#include "system/system_info.h"
#include "xiaoling_config.h"

#include <cJSON.h>
#include <esp_log.h>
#include <mbedtls/md5.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>
#include <vector>

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

// 直接使用 managed_components 的 HTTP 客户端完成鉴权 POST（沿用仓库内 ota/assets 的裸用方式）。
namespace {

bool PostJsonForToken(const std::string& url, const std::string& body,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      int& status_code, std::string& response_body,
                      int timeout_ms = 0) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network interface is unavailable");
        return false;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    http->SetHeader("Content-Type", "application/json");
    for (const auto& [name, value] : headers) {
        http->SetHeader(name.c_str(), value.c_str());
    }
    if (timeout_ms > 0) {
        http->SetTimeout(timeout_ms);
    }
    http->SetContent(std::string(body));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open POST request: %s", url.c_str());
        return false;
    }

    status_code = http->GetStatusCode();
    response_body = http->ReadAll();
    http->Close();
    return true;
}

bool GetJsonWithHeaders(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    int& status_code, std::string& response_body, int timeout_ms) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return false;
    }
    http->SetHeader("Content-Type", "application/json");
    for (const auto& [name, value] : headers) {
        http->SetHeader(name.c_str(), value.c_str());
    }
    if (timeout_ms > 0) {
        http->SetTimeout(timeout_ms);
    }
    if (!http->Open("GET", url)) {
        return false;
    }
    status_code = http->GetStatusCode();
    response_body = http->ReadAll();
    http->Close();
    return true;
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

bool XiaolingConn::ResolveKuwoMusicUrl(const std::string& item_id,
                                       const std::string& host,
                                       std::string& url) {
    url.clear();
    if (item_id.empty() || host.empty() || !EnsureAuthenticated()) {
        return false;
    }

    cJSON* request = cJSON_CreateObject();
    if (request == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(request, "itemid", item_id.c_str());
    cJSON_AddStringToObject(request, "format", "128kmp3");
    char* request_text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (request_text == nullptr) {
        return false;
    }

    const std::string request_body(request_text);
    cJSON_free(request_text);
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", NormalizeBearerToken(LoadAccessToken())},
        {"Device-Id", SystemInfo::GetCloudDeviceId()},
        {"Client-Id", Board::GetInstance().GetUuid()},
    };
    int status_code = 0;
    std::string response_body;
    const std::string url_endpoint =
        std::string("http://") + host + "/v1/kuwo/tranklink";
    if (!PostJsonForToken(url_endpoint, request_body, headers, status_code, response_body, 3000) ||
        status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "获取酷我歌曲 URL 失败: itemid=%s status=%d", item_id.c_str(), status_code);
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(response_body.data(), response_body.size());
    if (root == nullptr) {
        return false;
    }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON* first = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : nullptr;
    const cJSON* audio_path = cJSON_GetObjectItemCaseSensitive(first, "audiopath");
    if (cJSON_IsString(audio_path) && audio_path->valuestring != nullptr) {
        url = audio_path->valuestring;
        ESP_LOGI(TAG, "酷我歌曲 URL: itemid=%s, url=%s", item_id.c_str(),
                 url.c_str());
    }
    cJSON_Delete(root);
    return !url.empty();
}

bool XiaolingConn::FetchAlarmList(std::string& response_body) {
    response_body.clear();
    if (!EnsureAuthenticated()) {
        return false;
    }
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", NormalizeBearerToken(LoadAccessToken())},
        {"x-product-id", SystemInfo::GetProductId()},
        {"x-device-id", SystemInfo::GetCloudDeviceId()},
    };
    int status_code = 0;
    const std::string endpoint =
        std::string("https://") + XiaolingConfig::GetHost() + "/v1/alarm_clocks";
    if (!GetJsonWithHeaders(endpoint, headers, status_code, response_body, 10000) ||
        status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "同步云端闹钟列表失败: status=%d", status_code);
        response_body.clear();
        return false;
    }
    return true;
}

bool XiaolingConn::ResolveCustomAlarmDate(const std::string& alarm_id,
                                          std::string& date) {
    date.clear();
    if (alarm_id.empty() || !EnsureAuthenticated()) {
        return false;
    }
    cJSON* request = cJSON_CreateObject();
    if (request == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(request, "type", "custom");
    cJSON_AddStringToObject(request, "id", alarm_id.c_str());
    char* request_text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (request_text == nullptr) {
        return false;
    }
    const std::string request_body(request_text);
    cJSON_free(request_text);

    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", NormalizeBearerToken(LoadAccessToken())},
        {"x-product-id", SystemInfo::GetProductId()},
        {"x-device-id", SystemInfo::GetCloudDeviceId()},
    };
    int status_code = 0;
    std::string response_body;
    const std::string endpoint =
        std::string("https://") + XiaolingConfig::GetHost() + "/v1/date/transition";
    if (!PostJsonForToken(endpoint, request_body, headers, status_code, response_body, 10000) ||
        status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "查询 CUSTOM 闹钟下次日期失败: id=%s status=%d",
                 alarm_id.c_str(), status_code);
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(response_body.data(), response_body.size());
    if (root == nullptr) {
        return false;
    }
    const auto* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const bool success = (cJSON_IsNumber(code) && code->valueint == 0) ||
                         (cJSON_IsString(code) && code->valuestring != nullptr &&
                          std::strcmp(code->valuestring, "0") == 0);
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, "date");
    if (success && cJSON_IsString(value) && value->valuestring != nullptr) {
        date = value->valuestring;
    }
    cJSON_Delete(root);
    if (date.empty()) {
        ESP_LOGW(TAG, "CUSTOM 闹钟下次日期响应无效: id=%s", alarm_id.c_str());
        return false;
    }
    return true;
}

bool XiaolingConn::Authenticate(const std::string& extra_code) {
    const std::string url = XiaolingConfig::GetTokenUrl();
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

    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Activation-Version", "1"},
        {"Device-Id", device_id},
        {"Client-Id", Board::GetInstance().GetUuid()},
        {"User-Agent", SystemInfo::GetUserAgent()},
        {"Accept-Language", Lang::CODE},
    };
    ESP_LOGI(TAG, "小聆 Token 请求: POST %s, product_id_len=%u, device_id_len=%u, body_bytes=%u",
             url.c_str(), static_cast<unsigned>(product_id.size()),
             static_cast<unsigned>(device_id.size()),
             static_cast<unsigned>(request_body.size()));
    ESP_LOGI(TAG, "小聆 Token 请求内容: %s", request_body.c_str());
    int status_code = 0;
    std::string response_body;
    const bool request_ok = PostJsonForToken(url, request_body, headers,
                                             status_code, response_body);
    ESP_LOGI(TAG, "小聆 Token 响应: transport_ok=%d, status=%d, body_bytes=%u",
             request_ok ? 1 : 0, status_code,
             static_cast<unsigned>(response_body.size()));
    ESP_LOGI(TAG, "小聆 Token 响应内容: %s", response_body.c_str());
    if (!request_ok) {
        ESP_LOGE(TAG, "小聆鉴权请求失败: status=%d", status_code);
        SetError(XiaolingError::ServerNotConnected);
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(response_body.data(), response_body.size());
    if (root == nullptr) {
        ESP_LOGE(TAG, "小聆鉴权响应不是有效 JSON: status=%d", status_code);
        SetError(XiaolingError::ServerNotConnected);
        return false;
    }
    const auto* token = cJSON_GetObjectItemCaseSensitive(root, "token");
    std::string access_token;
    if (cJSON_IsString(token) && token->valuestring != nullptr) {
        access_token = token->valuestring;
    }
    const auto* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const auto* message = cJSON_GetObjectItemCaseSensitive(root, "message");
    char code_number[24] = {};
    const char* code_text = "<none>";
    if (cJSON_IsString(code) && code->valuestring != nullptr) {
        code_text = code->valuestring;
    } else if (cJSON_IsNumber(code)) {
        std::snprintf(code_number, sizeof(code_number), "%d", code->valueint);
        code_text = code_number;
    }
    const bool authentication_failed =
        cJSON_IsNumber(code) && code->valueint == 20104;
    ESP_LOGI(TAG, "小聆 Token 响应字段: code=%s, message=%s, token_len=%u",
             code_text,
             cJSON_IsString(message) && message->valuestring != nullptr
                 ? message->valuestring
                 : "<none>",
             static_cast<unsigned>(access_token.size()));
    cJSON_Delete(root);
    if (authentication_failed) {
        ESP_LOGE(TAG, "小聆云端鉴权失败: code=20104，请检查 PID/SID");
        SetError(XiaolingError::AuthenticationFailed);
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "小聆鉴权失败: status=%d", status_code);
        if (!authentication_failed) {
            SetError(XiaolingError::ServerNotConnected);
        }
        return false;
    }
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

std::string XiaolingConn::GetHttpAuthorization() const {
    return NormalizeBearerToken(LoadAccessToken());
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
