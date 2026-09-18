#include "qrcode_model.h"

#include "ui/tools/image/lvgl_image.h"

#if CONFIG_LSPLATFORM
#include "board.h"
#include "image_fetcher.h"
#endif

#include <new>
#include <utility>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "QrCodeModel"

namespace {

struct FetchRequest {
    QrCodeModel* model;
    std::string url;
    uint32_t generation;
};

}  // namespace

QrCodeModel& QrCodeModel::GetInstance() {
    static QrCodeModel instance;
    return instance;
}

void QrCodeModel::HandleSettingMessage(const cJSON* data) {
    const auto* guide = cJSON_IsObject(data)
                            ? cJSON_GetObjectItemCaseSensitive(data, "mp_guide")
                            : nullptr;
    const auto* role_config = cJSON_IsObject(guide)
                                  ? cJSON_GetObjectItemCaseSensitive(guide, "role_config")
                                  : nullptr;
    const auto* image_url = cJSON_IsObject(role_config)
                                ? cJSON_GetObjectItemCaseSensitive(role_config, "image_url")
                                : nullptr;
    if (!cJSON_IsString(image_url) || image_url->valuestring == nullptr ||
        image_url->valuestring[0] == '\0') {
        return;
    }
    SetRoleConfigUrl(image_url->valuestring);
}

void QrCodeModel::SetRoleConfigUrl(std::string url) {
    if (url.empty()) {
        return;
    }

    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto state = Snapshot();
        if (state.role_config_url == url &&
            (state.loading || state.role_config_image != nullptr)) {
            return;
        }

        generation = ++fetch_generation_;
        Update([&](QrCodeModelState& state) {
            state.role_config_url = url;
            state.role_config_image.reset();
            state.loading = true;
            state.generation = generation;
        });
    }

#if CONFIG_LSPLATFORM
    auto* request = new (std::nothrow) FetchRequest{
        .model = this,
        .url = url,
        .generation = generation,
    };
    if (request == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate role config image fetch request");
        CompleteFetch(url, generation, nullptr);
        return;
    }

    if (xTaskCreate(&QrCodeModel::FetchTask, "qrcode_fetch", 6144, request, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create role config image fetch task");
        const std::string failed_url = request->url;
        delete request;
        CompleteFetch(failed_url, generation, nullptr);
    }
#else
    CompleteFetch(url, generation, nullptr);
#endif
}

void QrCodeModel::ClearRoleConfigUrl() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    const auto state = Snapshot();
    if (state.role_config_url.empty() && state.role_config_image == nullptr && !state.loading) {
        return;
    }
    const auto generation = ++fetch_generation_;
    Update([generation](QrCodeModelState& state) {
        state.role_config_url.clear();
        state.role_config_image.reset();
        state.loading = false;
        state.generation = generation;
    });
}

void QrCodeModel::SelectRoleConfig() {
    Update([](QrCodeModelState& state) {
        state.content_source = QrCodeContentSource::kRoleConfig;
        state.custom_image.reset();
        state.custom_message.clear();
    });
}

void QrCodeModel::SetCustomQrCode(std::unique_ptr<LvglImage> image, std::string message) {
    if (image == nullptr) {
        return;
    }
    auto shared_image = std::shared_ptr<LvglImage>(std::move(image));
    Update([image = std::move(shared_image), message = std::move(message)](
               QrCodeModelState& state) mutable {
        state.custom_image = std::move(image);
        state.custom_message = std::move(message);
        state.content_source = QrCodeContentSource::kCustom;
    });
}

void QrCodeModel::FetchTask(void* arg) {
#if CONFIG_LSPLATFORM
    {
        std::unique_ptr<FetchRequest> request(static_cast<FetchRequest*>(arg));
        std::unique_ptr<LvglImage> image;
        auto* fetcher = Board::GetInstance().GetImageFetcher();
        if (fetcher != nullptr) {
            image = fetcher->Fetch(request->url);
        }
        request->model->CompleteFetch(request->url, request->generation, std::move(image));
    }
#else
    delete static_cast<FetchRequest*>(arg);
#endif
    vTaskDelete(nullptr);
}

void QrCodeModel::CompleteFetch(const std::string& url, uint32_t generation,
                                std::unique_ptr<LvglImage> image) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    const auto state = Snapshot();
    if (state.generation != generation || state.role_config_url != url) {
        return;
    }

    std::shared_ptr<LvglImage> cached_image;
    if (image != nullptr) {
        cached_image = std::shared_ptr<LvglImage>(std::move(image));
        ESP_LOGI(TAG, "Role config image cached");
    } else {
        ESP_LOGW(TAG, "Failed to cache role config image");
    }
    Update([&](QrCodeModelState& state) {
        state.role_config_image = std::move(cached_image);
        state.loading = false;
    });
}
