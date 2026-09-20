#include "ota.h"
#include "system/system_info.h"
#include "settings.h"
#include "ui/i18n/lang_config.h"
#include "xiaoling/conn/xiaoling_config.h"
#include "base64_codec.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_timer.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include <mbedtls/md5.h>

#define TAG "Ota"

namespace {

bool IsAppPackageEndpoint(const std::string& url) {
    return url.find("/v1/ota/packages") != std::string::npos;
}

std::string CalculateMd5Hex(const std::string& input) {
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

uint32_t GetVersionNumber(const std::string& version) {
    const auto parts = [&version]() {
        std::vector<uint32_t> result;
        std::stringstream stream(version);
        std::string segment;
        while (std::getline(stream, segment, '.')) {
            if (segment.empty() || !std::all_of(segment.begin(), segment.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                })) {
                return std::vector<uint32_t>();
            }
            try {
                result.push_back(static_cast<uint32_t>(std::stoul(segment)));
            } catch (...) {
                return std::vector<uint32_t>();
            }
        }
        return result;
    }();

    if (parts.empty() || parts.size() > 3 || parts[0] > 999 ||
        (parts.size() > 1 && parts[1] > 999) || (parts.size() > 2 && parts[2] > 999)) {
        return 0;
    }
    return parts[0] * 1000000U + (parts.size() > 1 ? parts[1] * 1000U : 0U) +
           (parts.size() > 2 ? parts[2] : 0U);
}

}  // namespace

// The device configuration endpoint is only used by LSPlatform builds.
#ifdef CONFIG_LSPLATFORM
namespace {

std::string BuildResourceConfigParam() {
    cJSON* params = cJSON_CreateObject();
    if (params == nullptr) {
        return {};
    }

    cJSON* firmware_info = cJSON_CreateObject();
    if (firmware_info == nullptr) {
        cJSON_Delete(params);
        return {};
    }

    cJSON_AddStringToObject(firmware_info, "type", "arcs-mini");
    cJSON_AddStringToObject(firmware_info, "version", esp_app_get_description()->version);
    cJSON_AddItemToObject(params, "firmware_info", firmware_info);

    char* params_json = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (params_json == nullptr) {
        return {};
    }

    const std::string encoded = Base64Codec::Encode(params_json);
    cJSON_free(params_json);
    return encoded;
}

std::string BuildResourceConfigUrl() {
    std::string url = std::string("http://") + XiaolingConfig::GetHost() +
                      "/external/device/configurations?product_id=" +
                      SystemInfo::GetProductId() + "&device_id=" +
                      SystemInfo::GetCloudDeviceId();
    ESP_LOGI(TAG, "Resource OTA environment: %s host=%s",
             XiaolingConfig::GetEnvironmentName(), XiaolingConfig::GetHost());
    const auto param = BuildResourceConfigParam();
    if (!param.empty()) {
        url += "&param=" + param;
    } else {
        ESP_LOGW(TAG, "Failed to build resource config param; requesting without param");
    }
    return url;
}

}  // namespace
#endif // CONFIG_LSPLATFORM


Ota::Ota() {
    current_version_ = esp_app_get_description()->version;
    firmware_version_ = current_version_;
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
#ifdef CONFIG_LSPLATFORM
        url = std::string("https://") + XiaolingConfig::GetHost() + "/v1/ota/packages";
#else
        url = "https://api.listenai.com/v1/xiaoling/" + SystemInfo::GetProductId() + "/ota/";
#endif
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (network == nullptr) {
        return nullptr;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return nullptr;
    }
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/*
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    current_version_ = esp_app_get_description()->version;
    firmware_version_ = current_version_;
    firmware_url_.clear();
    firmware_size_ = 0;
    firmware_description_.clear();
    has_new_version_ = false;

    const std::string url = GetCheckVersionUrl();
    if (url.size() < 10) {
        ESP_LOGE(TAG, "Invalid OTA check URL");
        return ESP_ERR_INVALID_ARG;
    }

    auto http = SetupHttp();
    if (http == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

#ifdef CONFIG_LSPLATFORM
    const bool app_package_api = IsAppPackageEndpoint(url);
#else
    const bool app_package_api = false;
#endif
    std::string request;
    std::string method;
    const auto parse_firmware_details = [this](const cJSON* object) {
        const auto* size = cJSON_GetObjectItemCaseSensitive(object, "size");
        if (cJSON_IsNumber(size) && size->valuedouble > 0) {
            firmware_size_ = static_cast<size_t>(size->valuedouble);
        }

        const char* description_keys[] = {"description", "release_notes", "changelog"};
        for (const char* key : description_keys) {
            const auto* description = cJSON_GetObjectItemCaseSensitive(object, key);
            if (cJSON_IsString(description) && description->valuestring != nullptr &&
                description->valuestring[0] != '\0') {
                firmware_description_ = description->valuestring;
                break;
            }
        }
    };

    if (app_package_api) {
        const std::string product_id = SystemInfo::GetProductId();
        const std::string device_id = SystemInfo::GetCloudDeviceId();
        const std::string secret_id = SystemInfo::GetSecretId();
        if (product_id.empty() || device_id.empty() || secret_id.empty()) {
            ESP_LOGE(TAG, "App OTA package request missing product/device/secret identity");
            return ESP_ERR_INVALID_STATE;
        }

        const std::string current_time = std::to_string(static_cast<long long>(std::time(nullptr)));
        const std::string checksum = CalculateMd5Hex(secret_id + device_id + current_time);
        if (checksum.empty()) {
            ESP_LOGE(TAG, "Failed to calculate app OTA request checksum");
            return ESP_FAIL;
        }
        http->SetHeader("Accept", "application/json");
        http->SetHeader("X-ProductID", product_id);
        http->SetHeader("X-DeviceID", device_id);
        http->SetHeader("X-CurTime", current_time);
        http->SetHeader("X-CheckSum", checksum);
        method = "GET";
    } else {
        request = board.GetSystemInfoJson();
        method = request.empty() ? "GET" : "POST";
    }
    ESP_LOGI(TAG, "Firmware OTA request: method=%s url=%s body=%s",
             method.c_str(), url.c_str(), request.empty() ? "<empty>" : request.c_str());
    if (!request.empty()) {
        http->SetContent(std::move(request));
    }
    if (!http->Open(method, url)) {
        ESP_LOGE(TAG, "Firmware OTA request failed to open: url=%s", url.c_str());
        return ESP_FAIL;
    }
    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();
    ESP_LOGI(TAG, "Firmware OTA response: status=%d body=%s", status,
             body.empty() ? "<empty>" : body.c_str());
    if (status != 200) {
        ESP_LOGE(TAG, "OTA check returned HTTP %d", status);
        http->Close();
        return ESP_FAIL;
    }
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Invalid OTA check response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // The provisioning flow uses the activation payload returned by the same
    // endpoint as the OTA metadata. Keep parsing it even for LSPlatform
    // devices; the app binding QR code is shown after Wi-Fi provisioning.
    has_activation_code_ = false;
    has_activation_challenge_ = false;
    activation_message_.clear();
    activation_code_.clear();
    activation_challenge_.clear();
#ifdef CONFIG_LSPLATFORM
    has_activation_qrcode_ = false;
    activation_qrcode_.clear();
#endif

    const auto parse_activation = [this](const cJSON* response) {
        const auto* activation = cJSON_GetObjectItemCaseSensitive(response, "activation");
        if (!cJSON_IsObject(activation)) {
            return;
        }
        const auto* message = cJSON_GetObjectItemCaseSensitive(activation, "message");
        if (cJSON_IsString(message) && message->valuestring != nullptr) {
            activation_message_ = message->valuestring;
        }

        const auto* code = cJSON_GetObjectItemCaseSensitive(activation, "code");
        if (cJSON_IsString(code) && code->valuestring != nullptr) {
            activation_code_ = code->valuestring;
            has_activation_code_ = !activation_code_.empty();
        }

        const auto* challenge = cJSON_GetObjectItemCaseSensitive(activation, "challenge");
        if (cJSON_IsString(challenge) && challenge->valuestring != nullptr) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = !activation_challenge_.empty();
        }

        const auto* timeout_ms = cJSON_GetObjectItemCaseSensitive(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms) && timeout_ms->valueint > 0) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
#ifdef CONFIG_LSPLATFORM
        const auto* qrcode = cJSON_GetObjectItemCaseSensitive(activation, "qrcode");
        if (cJSON_IsString(qrcode) && qrcode->valuestring != nullptr) {
            activation_qrcode_ = qrcode->valuestring;
            has_activation_qrcode_ = !activation_qrcode_.empty();
        }
#endif
    };
    parse_activation(root);

    // /v1/ota/packages only carries application package metadata. Keep the
    // legacy request for provisioning activation data, which is still served
    // by /v1/xiaoling/<product_id>/ota/.
#ifdef CONFIG_LSPLATFORM
    if (app_package_api && !has_activation_code_ && !has_activation_challenge_) {
        const std::string activation_url = std::string("https://") + XiaolingConfig::GetHost() +
                                           "/v1/xiaoling/" + SystemInfo::GetProductId() + "/ota/";
        auto activation_http = SetupHttp();
        if (activation_http != nullptr) {
            std::string activation_request = board.GetSystemInfoJson();
            const std::string activation_method = activation_request.empty() ? "GET" : "POST";
            if (!activation_request.empty()) {
                activation_http->SetContent(std::move(activation_request));
            }
            if (activation_http->Open(activation_method, activation_url)) {
                const int activation_status = activation_http->GetStatusCode();
                const std::string activation_body = activation_http->ReadAll();
                activation_http->Close();
                if (activation_status == 200) {
                    cJSON* activation_root = cJSON_Parse(activation_body.c_str());
                    if (activation_root != nullptr) {
                        parse_activation(activation_root);
                        cJSON_Delete(activation_root);
                    }
                } else {
                    ESP_LOGW(TAG, "Activation metadata request returned HTTP %d", activation_status);
                }
            } else {
                ESP_LOGW(TAG, "Activation metadata request failed to open");
            }
        }
    }
#endif

    if (app_package_api) {
        const auto* download = cJSON_GetObjectItemCaseSensitive(root, "url");
        const auto* version = cJSON_GetObjectItemCaseSensitive(root, "version");
        const auto* version_number = cJSON_GetObjectItemCaseSensitive(root, "version_number");
        if (cJSON_IsString(download) && download->valuestring != nullptr &&
            download->valuestring[0] != '\0') {
            if (!cJSON_IsString(version) || version->valuestring == nullptr || version->valuestring[0] == '\0' ||
                !cJSON_IsNumber(version_number) || version_number->valuedouble < 0 ||
                version_number->valuedouble > static_cast<double>(UINT32_MAX) ||
                std::strlen(download->valuestring) >= 512) {
                ESP_LOGE(TAG, "App OTA package response is missing version/version_number");
            } else {
                firmware_version_ = version->valuestring;
                firmware_url_ = download->valuestring;
                parse_firmware_details(root);
                const uint32_t server_version_number = static_cast<uint32_t>(version_number->valuedouble);
                const uint32_t local_version_number = GetVersionNumber(current_version_);
                has_new_version_ = local_version_number != 0
                                        ? server_version_number > local_version_number
                                        : IsNewVersionAvailable(current_version_, firmware_version_);
                ESP_LOGI(TAG, "App OTA package: version=%s version_number=%u local_version_number=%u",
                         firmware_version_.c_str(), static_cast<unsigned int>(server_version_number),
                         static_cast<unsigned int>(local_version_number));
            }
        }
    } else {
        auto* firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
        if (cJSON_IsObject(firmware)) {
            auto* version = cJSON_GetObjectItemCaseSensitive(firmware, "version");
            auto* download = cJSON_GetObjectItemCaseSensitive(firmware, "url");
            if (cJSON_IsString(version) && cJSON_IsString(download) &&
                version->valuestring[0] != '\0' && download->valuestring[0] != '\0') {
                firmware_version_ = version->valuestring;
                firmware_url_ = download->valuestring;
                parse_firmware_details(firmware);
                has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
                auto* force = cJSON_GetObjectItemCaseSensitive(firmware, "force");
                if (cJSON_IsNumber(force) && force->valueint == 1) {
                    has_new_version_ = true;
                }
            }
        }
    }
    ESP_LOGI(TAG, "APP OTA: current=%s target=%s available=%s", current_version_.c_str(),
             firmware_version_.c_str(), has_new_version_ ? "yes" : "no");
    cJSON_Delete(root);
    return ESP_OK;
}

namespace {

bool ParseMd5(const cJSON* value, std::array<uint8_t, 16>& output) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr ||
        std::strlen(value->valuestring) != output.size() * 2) {
        return false;
    }
    for (size_t i = 0; i < output.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(value->valuestring[i * 2])) ||
            !std::isxdigit(static_cast<unsigned char>(value->valuestring[i * 2 + 1]))) {
            return false;
        }
        unsigned int byte = 0;
        if (std::sscanf(value->valuestring + i * 2, "%2x", &byte) != 1) {
            return false;
        }
        output[i] = static_cast<uint8_t>(byte);
    }
    return true;
}

bool ParseResourceInfo(const cJSON* object, Ota::ResourceInfo& resource) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    auto* size = cJSON_GetObjectItemCaseSensitive(object, "size");
    auto* md5 = cJSON_GetObjectItemCaseSensitive(object, "md5");
    auto* url = cJSON_GetObjectItemCaseSensitive(object, "url");
    if (!cJSON_IsNumber(size) || !cJSON_IsString(url) || url->valuestring[0] == '\0' ||
        !ParseMd5(md5, resource.md5)) {
        return false;
    }
    resource.available = true;
    resource.size = static_cast<size_t>(size->valuedouble);
    resource.url = url->valuestring;
    return resource.size != 0;
}

void ParseResources(const cJSON* root, Ota::ResourceInfo& wake_word,
                    Ota::ResourceInfo& tone, Ota::ResourceInfo& emoji) {
    const cJSON* data = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "data"))
                            ? cJSON_GetObjectItemCaseSensitive(root, "data") : root;
    const cJSON* wake = cJSON_GetObjectItemCaseSensitive(data, "wakeup_word");
    if (cJSON_IsObject(wake)) {
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(wake, "text");
        if (cJSON_IsString(text)) wake_word.wake_word = text->valuestring;
        const cJSON* resources = cJSON_GetObjectItemCaseSensitive(wake, "resources");
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, resources) {
            const cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (cJSON_IsString(name) && std::strcmp(name->valuestring, "wake_word.bin") == 0) {
                ParseResourceInfo(item, wake_word);
            }
        }
    }
    ParseResourceInfo(cJSON_GetObjectItemCaseSensitive(data, "prompt_tone"), tone);
    ParseResourceInfo(cJSON_GetObjectItemCaseSensitive(data, "emoji"), emoji);
}

}  // namespace

esp_err_t Ota::CheckResources() {
    wake_word_resource_ = {};
    tone_resource_ = {};
    emoji_resource_ = {};

    Settings settings("wifi", false);
    std::string url = settings.GetString("resource_ota_url");
#ifdef CONFIG_LSPLATFORM
    if (url.empty()) {
        url = BuildResourceConfigUrl();
    } else {
        ESP_LOGI(TAG, "Resource OTA using KV override URL: %s", url.c_str());
    }
#endif
    if (url.empty()) {
        return ESP_ERR_NOT_FOUND;
    }

    auto http = SetupHttp();
    if (http == nullptr) {
        ESP_LOGE(TAG, "Resource OTA request could not create HTTP client: url=%s", url.c_str());
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Resource OTA request: method=GET url=%s body=<empty>", url.c_str());
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Resource OTA request failed to open: url=%s", url.c_str());
        http->Close();
        return ESP_FAIL;
    }
    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();
    ESP_LOGI(TAG, "Resource OTA response: status=%d body=%s", status,
             body.empty() ? "<empty>" : body.c_str());
    http->Close();
    if (status != 200) {
        ESP_LOGW(TAG, "Resource OTA check returned HTTP %d: url=%s", status, url.c_str());
        return ESP_FAIL;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Resource OTA response is not valid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ParseResources(root, wake_word_resource_, tone_resource_, emoji_resource_);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Resource OTA packages: wake=%d tone=%d emoji=%d",
             wake_word_resource_.available, tone_resource_.available, emoji_resource_.available);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return false;
    }
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        http->Close();
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        http->Close();
        return false;
    }

    char buffer[4096];
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    bool ota_started = false;
    std::string pending;
    pending.reserve(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t));
    while (true) {
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            http->Close();
            if (ota_started) esp_ota_abort(update_handle);
            return false;
        }

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0) {
            break;
        }

        if (!ota_started) {
            pending.append(buffer, ret);
            if (pending.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info{};
                memcpy(&new_app_info, pending.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));

                auto current_version = esp_app_get_description()->version;
                ESP_LOGI(TAG, "Current version: %s, New version: %s", current_version, new_app_info.version);

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle) != ESP_OK) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    http->Close();
                    return false;
                }
                if (esp_ota_write(update_handle, pending.data(), pending.size()) != ESP_OK) {
                    esp_ota_abort(update_handle);
                    http->Close();
                    return false;
                }
                ota_started = true;
                pending.clear();
            }
        } else {
            auto err = esp_ota_write(update_handle, buffer, ret);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                http->Close();
                return false;
            }
        }
    }
    http->Close();

    if (!ota_started) {
        ESP_LOGE(TAG, "Firmware image ended before its header was received");
        return false;
    }

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}


std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;

    while (std::getline(ss, segment, '.')) {
        if (segment.empty() || !std::all_of(segment.begin(), segment.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            return {};
        }
        try {
            versionNumbers.push_back(std::stoi(segment));
        } catch (...) {
            return {};
        }
    }

    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);

    if (current.empty() || newer.empty()) {
        ESP_LOGW(TAG, "Invalid firmware version: current=%s target=%s",
                 currentVersion.c_str(), newVersion.c_str());
        return false;
    }

    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }

    return newer.size() > current.size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节

    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    // The package API is dedicated to application OTA metadata. Device
    // binding still uses the legacy product OTA endpoint.
    if (IsAppPackageEndpoint(url)) {
        url = std::string("https://") + XiaolingConfig::GetHost() + "/v1/xiaoling/" +
              SystemInfo::GetProductId() + "/ota/";
    }
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }

    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
