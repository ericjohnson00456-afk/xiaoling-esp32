#include "ota.h"

#include "base64_codec.h"
#include "resource/resource.h"
#include "system/settings.h"
#include "system/system_info.h"
#include "xiaoling/conn/xiaoling_config.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <mbedtls/md5.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#define TAG "OtaResource"

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
        if (cJSON_IsString(text)) {
            wake_word.wake_word = text->valuestring;
        }
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

ResourcePackage ToResourcePackage(Ota::Resource resource) {
    switch (resource) {
        case Ota::Resource::Tone: return ResourcePackage::Tone;
        case Ota::Resource::Emoji: return ResourcePackage::Emoji;
        case Ota::Resource::WakeWord: return ResourcePackage::WakeWord;
    }
    return ResourcePackage::Tone;
}

}  // namespace

esp_err_t Ota::CheckResources() {
    wake_word_resource_ = {};
    tone_resource_ = {};
    emoji_resource_ = {};

    Settings settings("wifi", false);
    std::string url = settings.GetString("resource_ota_url");
    if (url.empty()) {
        url = BuildResourceConfigUrl();
    } else {
        ESP_LOGI(TAG, "Resource OTA using KV override URL: %s", url.c_str());
    }
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

bool Ota::VerifyResource(Resource resource, size_t expected_size,
                         const std::array<uint8_t, 16>& expected_md5) {
    const esp_partition_t* partition =
        ::Resource::GetPartition(ToResourcePackage(resource));
    if (partition == nullptr || expected_size == 0 || expected_size > partition->size) {
        ESP_LOGE(TAG, "Invalid resource partition or expected size: %u",
                 static_cast<unsigned>(expected_size));
        return false;
    }

    constexpr size_t kBufferSize = 4096;
    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[kBufferSize]);
    if (!buffer) {
        return false;
    }

    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    size_t offset = 0;
    bool success = true;
    while (offset < expected_size) {
        const size_t chunk = std::min(kBufferSize, expected_size - offset);
        if (esp_partition_read(partition, offset, buffer.get(), chunk) != ESP_OK) {
            success = false;
            break;
        }
        mbedtls_md5_update(&md5, buffer.get(), chunk);
        offset += chunk;
    }
    std::array<uint8_t, 16> actual{};
    mbedtls_md5_finish(&md5, actual.data());
    mbedtls_md5_free(&md5);
    success = success && actual == expected_md5;
    ESP_LOGI(TAG, "Resource %s (%u bytes)", success ? "is current" : "needs update",
             static_cast<unsigned>(expected_size));
    return success;
}

bool Ota::UpgradeResource(Resource resource, const std::string& url, size_t expected_size,
                          const std::array<uint8_t, 16>& expected_md5,
                          std::function<void(int progress, size_t speed)> callback) {
    const ResourcePackage package = ToResourcePackage(resource);
    const esp_partition_t* partition = ::Resource::GetPartition(package);
    if (partition == nullptr) {
        ESP_LOGE(TAG, "Split resource partition is unavailable");
        return false;
    }
    ::Resource::Unload(package);

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ::Resource::Reload(package);
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Resource download request could not create HTTP client: url=%s", url.c_str());
        ::Resource::Reload(package);
        return false;
    }
    ESP_LOGI(TAG, "Resource download request: method=GET url=%s body=<binary/empty>", url.c_str());
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Resource download request failed to open: url=%s", url.c_str());
        ::Resource::Reload(package);
        return false;
    }
    const int status = http->GetStatusCode();
    const size_t content_length = http->GetBodyLength();
    ESP_LOGI(TAG, "Resource download response: status=%d content_length=%u url=%s",
             status, static_cast<unsigned>(content_length), url.c_str());
    if (status != 200) {
        ESP_LOGE(TAG, "Resource download returned HTTP %d: url=%s", status, url.c_str());
        http->Close();
        ::Resource::Reload(package);
        return false;
    }
    if (content_length == 0 || content_length > partition->size ||
        (expected_size != 0 && content_length != expected_size)) {
        ESP_LOGE(TAG, "Invalid resource size: %u", static_cast<unsigned>(content_length));
        http->Close();
        ::Resource::Reload(package);
        return false;
    }

    constexpr size_t kBufferSize = 4096;
    constexpr size_t kSectorSize = 4096;
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[kBufferSize]);
    if (!buffer) {
        http->Close();
        ::Resource::Reload(package);
        return false;
    }
    const size_t erase_size = (content_length + kSectorSize - 1) / kSectorSize * kSectorSize;
    if (esp_partition_erase_range(partition, 0, erase_size) != ESP_OK) {
        http->Close();
        ::Resource::Reload(package);
        return false;
    }

    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    size_t written = 0;
    size_t recent_read = 0;
    int64_t last_report_time = esp_timer_get_time();
    while (written < content_length) {
        const int ret = http->Read(buffer.get(), std::min(kBufferSize, content_length - written));
        if (ret <= 0 || esp_partition_write(partition, written, buffer.get(), ret) != ESP_OK) {
            ESP_LOGE(TAG, "Resource download/write failed at %u", static_cast<unsigned>(written));
            mbedtls_md5_free(&md5);
            http->Close();
            // The partition has already been erased. Leave it unmounted; the
            // application will reboot immediately and retry the resource OTA.
            return false;
        }
        mbedtls_md5_update(&md5, reinterpret_cast<const uint8_t*>(buffer.get()), ret);
        written += static_cast<size_t>(ret);
        recent_read += static_cast<size_t>(ret);
        const int64_t now = esp_timer_get_time();
        if (callback && (now - last_report_time >= 1000000 || written == content_length)) {
            callback(static_cast<int>(written * 100 / content_length), recent_read);
            last_report_time = now;
            recent_read = 0;
        }
    }
    http->Close();

    std::array<uint8_t, 16> actual{};
    mbedtls_md5_finish(&md5, actual.data());
    mbedtls_md5_free(&md5);
    if (expected_size != 0 && actual != expected_md5) {
        ESP_LOGE(TAG, "Downloaded resource MD5 mismatch");
        // Do not expose a corrupt ROMFS image to runtime consumers.
        return false;
    }
    return ::Resource::Reload(package);
}
