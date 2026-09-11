#include "banners.h"

#include <cstring>
#include <esp_log.h>

#include "cloud/cloud_http.h"
#include "cJSON.h"
#include "xiaoling/xiaoling_auth.h"

#define TAG "Banners"

bool Banners::Fetch(std::string url) {
    const auto auth = XiaolingAuth::GetInstance().Load();
    if (auth.access_token.empty()) {
        ESP_LOGE(TAG, "Failed to get cloud authorization token");
        return false;
    }

    const CloudHttpHeaders headers = {
        {"Authorization", XiaolingAuth::NormalizeBearerToken(auth.access_token)},
        {"Content-Type", "application/json"},
    };
    CloudHttpResponse response;
    CloudHttp http;
    if (!http.Get(url, response, headers, 5000)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection to %s", url.c_str());
        return false;
    }

    if (response.status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status code: %d", response.status_code);
        return false;
    }

    if (response.body.empty()) {
        ESP_LOGE(TAG, "Failed to read response body");
        return false;
    }

    cJSON* root = cJSON_Parse(response.body.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response: %s", cJSON_GetErrorPtr());
        return false;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data && cJSON_IsObject(data)) {
        cJSON* banners = cJSON_GetObjectItem(data, "banners");
        if (banners && cJSON_IsArray(banners)) {
            banners_.clear();
            current_index_ = 0;
            int size = cJSON_GetArraySize(banners);
            for (int i = 0; i < size; i++) {
                cJSON* banner = cJSON_GetArrayItem(banners, i);
                if (banner && cJSON_IsObject(banner)) {
                    cJSON* text = cJSON_GetObjectItem(banner, "text");
                    if (text && cJSON_IsString(text) && text->valuestring) {
                        banners_.push_back(std::string(text->valuestring));
                    } else {
                        ESP_LOGW(TAG, "Banner item %d has no valid text field", i);
                    }
                }
            }
            ESP_LOGI(TAG, "Parsed %d banners from response", size);
        } else {
            ESP_LOGE(TAG, "Response data contains no valid banners array");
        }
    } else {
        ESP_LOGE(TAG, "Response contains no valid data object");
    }

    cJSON_Delete(root);

    return true;
}

std::string Banners::Next() {
    if (banners_.empty()) {
        return "";
    }
    std::string banner = banners_[current_index_];
    current_index_ = (current_index_ + 1) % banners_.size();
    return banner;
}
