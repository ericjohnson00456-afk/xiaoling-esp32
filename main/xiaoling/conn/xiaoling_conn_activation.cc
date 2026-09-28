#include "xiaoling_conn_activation.h"

#include "board.h"
#include "system/system_info.h"
#include "ui/i18n/lang_config.h"
#include "xiaoling_config.h"

#include <cJSON.h>
#include <esp_log.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstdio>
#include <memory>
#include <utility>

#define TAG "XiaolingActivation"

namespace {

std::unique_ptr<Http> CreateActivationHttp(const std::string& serial_number) {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (network == nullptr) {
        return nullptr;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return nullptr;
    }

    http->SetHeader("Activation-Version", serial_number.empty() ? "1" : "2");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Client-Id", board.GetUuid());
    if (!serial_number.empty()) {
        http->SetHeader("Serial-Number", serial_number);
    }
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");
    return http;
}

std::string BuildActivationPayload(const std::string& serial_number,
                                   const std::string& challenge) {
    if (serial_number.empty()) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32] = {};
    const esp_err_t result = esp_hmac_calculate(
        HMAC_KEY0, reinterpret_cast<const uint8_t*>(challenge.data()),
        challenge.size(), hmac_result);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "激活 HMAC 计算失败: %s", esp_err_to_name(result));
        return "{}";
    }
    char byte_text[3] = {};
    for (const uint8_t byte : hmac_result) {
        std::snprintf(byte_text, sizeof(byte_text), "%02x", byte);
        hmac_hex += byte_text;
    }
#endif

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number.c_str());
    cJSON_AddStringToObject(payload, "challenge", challenge.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    char* json_text = cJSON_PrintUnformatted(payload);
    const std::string json = json_text != nullptr ? json_text : "{}";
    cJSON_free(json_text);
    cJSON_Delete(payload);
    return json;
}

}  // namespace

XiaolingConnActivation::XiaolingConnActivation()
    : serial_number_(SystemInfo::GetDeviceSerialNumber()) {
}

bool XiaolingConnActivation::Fetch() {
    message_.clear();
    code_.clear();
    challenge_.clear();
    qrcode_.clear();

    const std::string url = std::string("https://") + XiaolingConfig::GetHost() +
                            "/v1/xiaoling/" + SystemInfo::GetProductId() + "/ota/";
    auto http = CreateActivationHttp(serial_number_);
    if (http == nullptr) {
        return false;
    }

    std::string request = Board::GetInstance().GetSystemInfoJson();
    const char* method = request.empty() ? "GET" : "POST";
    if (!request.empty()) {
        http->SetContent(std::move(request));
    }
    ESP_LOGI(TAG, "小聆激活信息请求: method=%s url=%s", method, url.c_str());
    if (!http->Open(method, url)) {
        ESP_LOGW(TAG, "小聆激活信息请求失败");
        return false;
    }

    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();
    http->Close();
    if (status != 200) {
        ESP_LOGW(TAG, "小聆激活信息请求返回 HTTP %d: %s", status,
                 body.empty() ? "<empty>" : body.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "小聆激活信息响应不是有效 JSON");
        return false;
    }
    const cJSON* activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
    if (!cJSON_IsObject(activation)) {
        activation = root;
    }
    const auto read_string = [activation](const char* key, std::string& target) {
        const cJSON* value = cJSON_GetObjectItemCaseSensitive(activation, key);
        if (cJSON_IsString(value) && value->valuestring != nullptr) {
            target = value->valuestring;
        }
    };
    read_string("message", message_);
    read_string("code", code_);
    read_string("challenge", challenge_);
    read_string("qrcode", qrcode_);
    cJSON_Delete(root);
    return true;
}

esp_err_t XiaolingConnActivation::Submit() const {
    if (challenge_.empty()) {
        return ESP_ERR_INVALID_STATE;
    }

    const std::string url = std::string("https://") + XiaolingConfig::GetHost() +
                            "/v1/xiaoling/" + SystemInfo::GetProductId() +
                            "/ota/activate";
    auto http = CreateActivationHttp(serial_number_);
    if (http == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    http->SetContent(BuildActivationPayload(serial_number_, challenge_));
    if (!http->Open("POST", url)) {
        ESP_LOGW(TAG, "小聆激活请求失败");
        return ESP_FAIL;
    }

    const int status = http->GetStatusCode();
    if (status == 202) {
        http->Close();
        return ESP_ERR_TIMEOUT;
    }
    if (status != 200) {
        const std::string body = http->ReadAll();
        http->Close();
        ESP_LOGW(TAG, "小聆激活请求返回 HTTP %d: %s", status,
                 body.empty() ? "<empty>" : body.c_str());
        return ESP_FAIL;
    }
    http->Close();
    return ESP_OK;
}
