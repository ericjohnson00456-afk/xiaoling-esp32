#include "ota.h"
#include "system/system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_timer.h>

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include <mbedtls/md5.h>

#define TAG "OtaFirmware"

namespace {

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

/*
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
    current_version_ = esp_app_get_description()->version;
    firmware_version_ = current_version_;
    firmware_url_.clear();
    firmware_size_ = 0;
    firmware_description_.clear();
    has_new_version_ = false;

    const std::string url = GetCheckVersionUrl();
    if (url.size() < 10) {
        ESP_LOGE(TAG, "Invalid Xiaoling OTA check URL");
        return ESP_ERR_INVALID_ARG;
    }

    const std::string product_id = SystemInfo::GetProductId();
    const std::string device_id = SystemInfo::GetCloudDeviceId();
    const std::string secret_id = SystemInfo::GetSecretId();
    if (product_id.empty() || device_id.empty() || secret_id.empty()) {
        ESP_LOGE(TAG, "Xiaoling OTA request missing product/device/secret identity");
        return ESP_ERR_INVALID_STATE;
    }

    auto http = SetupHttp();
    if (http == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const std::string current_time =
        std::to_string(static_cast<long long>(std::time(nullptr)));
    const std::string checksum = CalculateMd5Hex(secret_id + device_id + current_time);
    if (checksum.empty()) {
        ESP_LOGE(TAG, "Failed to calculate Xiaoling OTA request checksum");
        return ESP_FAIL;
    }
    http->SetHeader("Accept", "application/json");
    http->SetHeader("X-ProductID", product_id);
    http->SetHeader("X-DeviceID", device_id);
    http->SetHeader("X-CurTime", current_time);
    http->SetHeader("X-CheckSum", checksum);
    ESP_LOGI(TAG,
             "Firmware OTA headers: X-ProductID=%s X-DeviceID=%s X-CurTime=%s X-CheckSum=%s",
             product_id.c_str(), device_id.c_str(), current_time.c_str(), checksum.c_str());
    ESP_LOGI(TAG, "Firmware OTA request: method=GET url=%s body=<empty>", url.c_str());
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Firmware OTA request failed to open: url=%s", url.c_str());
        return ESP_FAIL;
    }

    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Firmware OTA response: status=%d body=%s", status,
             body.empty() ? "<empty>" : body.c_str());
    if (status != 200) {
        ESP_LOGE(TAG, "OTA check returned HTTP %d", status);
        return ESP_FAIL;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Invalid Xiaoling OTA response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const auto* download = cJSON_GetObjectItemCaseSensitive(root, "url");
    const auto* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const auto* version_number = cJSON_GetObjectItemCaseSensitive(root, "version_number");
    if (cJSON_IsString(download) && download->valuestring != nullptr &&
        download->valuestring[0] != '\0') {
        if (!cJSON_IsString(version) || version->valuestring == nullptr ||
            version->valuestring[0] == '\0' || !cJSON_IsNumber(version_number) ||
            version_number->valuedouble < 0 ||
            version_number->valuedouble > static_cast<double>(UINT32_MAX) ||
            std::strlen(download->valuestring) >= 512) {
            ESP_LOGE(TAG, "Xiaoling OTA response is missing version/version_number");
        } else {
            firmware_version_ = version->valuestring;
            firmware_url_ = download->valuestring;

            const auto* size = cJSON_GetObjectItemCaseSensitive(root, "size");
            if (cJSON_IsNumber(size) && size->valuedouble > 0) {
                firmware_size_ = static_cast<size_t>(size->valuedouble);
            }
            const char* description_keys[] = {
                "description", "release_notes", "changelog"
            };
            for (const char* key : description_keys) {
                const auto* description = cJSON_GetObjectItemCaseSensitive(root, key);
                if (cJSON_IsString(description) && description->valuestring != nullptr &&
                    description->valuestring[0] != '\0') {
                    firmware_description_ = description->valuestring;
                    break;
                }
            }

            const uint32_t server_version_number =
                static_cast<uint32_t>(version_number->valuedouble);
            const uint32_t local_version_number = GetVersionNumber(current_version_);
            if (local_version_number == 0) {
                ESP_LOGE(TAG, "Invalid local firmware version: %s",
                         current_version_.c_str());
            } else {
                has_new_version_ = server_version_number > local_version_number;
            }
            ESP_LOGI(TAG,
                     "Xiaoling OTA package: version=%s version_number=%u local_version_number=%u",
                     firmware_version_.c_str(),
                     static_cast<unsigned int>(server_version_number),
                     static_cast<unsigned int>(local_version_number));
        }
    }

    ESP_LOGI(TAG, "APP OTA: current=%s target=%s available=%s",
             current_version_.c_str(), firmware_version_.c_str(),
             has_new_version_ ? "yes" : "no");
    cJSON_Delete(root);
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
