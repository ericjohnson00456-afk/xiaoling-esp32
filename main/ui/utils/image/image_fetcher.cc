#include "image_fetcher.h"

#include <cstring>

#include <esp_log.h>
#include <esp_app_desc.h>
#include <jpeg_decoder.h>

#include "board.h"
#include "network_interface.h"

#define TAG "ImageFetcher"

std::unique_ptr<LvglImage> ImageFetcher::Fetch(const std::string& url, int timeout_ms) {
    int ret;

    ESP_LOGI(TAG, "Fetching image from %s", url.c_str());

    try {
        auto network = board_->GetNetwork();
        if (!network) {
            ESP_LOGE(TAG, "Failed to get network instance");
            return nullptr;
        }

        auto http = network->CreateHttp();
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            return nullptr;
        }

        http->SetTimeout(timeout_ms);

        auto app_desc = esp_app_get_description();
        auto user_agent = std::string(BOARD_NAME "/") + app_desc->version;
        http->SetHeader("User-Agent", user_agent);

        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "Failed to connect to %s", url.c_str());
            return nullptr;
        }

        int status_code = http->GetStatusCode();
        if (status_code != 200) {
            ESP_LOGE(TAG, "Failed to fetch image, status code: %d", status_code);
            http->Close();
            return nullptr;
        }

        std::string image_data = http->ReadAll();
        http->Close();

        if (image_data.empty()) {
            ESP_LOGE(TAG, "No image data received");
            return nullptr;
        }

        ESP_LOGI(TAG, "Downloaded image data: %u bytes", image_data.size());

        if (image_data.size() > 5 * 1024 * 1024) {
            ESP_LOGE(TAG, "Image data too large");
            return nullptr;
        }

        if (image_data.size() < 10 ||
            image_data[0] != 0xFF || image_data[1] != 0xD8) {
            ESP_LOGE(TAG, "Invalid JPEG header");
            return nullptr;
        }

        esp_jpeg_image_cfg_t config = {
            .indata = reinterpret_cast<uint8_t*>(image_data.data()),
            .indata_size = static_cast<uint32_t>(image_data.size()),
            .outbuf = nullptr,
            .outbuf_size = 0,
            .out_format = JPEG_IMAGE_FORMAT_RGB565,
            .out_scale = JPEG_IMAGE_SCALE_0,
        };
        esp_jpeg_image_output_t out_info = {};
        ret = esp_jpeg_get_image_info(&config, &out_info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get JPEG header info: %s", esp_err_to_name(ret));
            return nullptr;
        }

        ESP_LOGI(TAG, "JPEG header info: width=%d, height=%d", out_info.width, out_info.height);

        if (out_info.width == 0 || out_info.height == 0 ||
            out_info.width > 2048 || out_info.height > 2048) {
            ESP_LOGE(TAG, "Invalid JPEG dimensions: %dx%d", out_info.width, out_info.height);
            return nullptr;
        }

        size_t out_size = out_info.output_len;
        if (out_size > 2 * 1024 * 1024) {
            ESP_LOGE(TAG, "Decoded image too large");
            return nullptr;
        }

        uint8_t* out_data = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            16, out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!out_data) {
            ESP_LOGE(TAG, "Failed to allocate decoded image in PSRAM, size: %d bytes", (int)out_size);
            return nullptr;
        }

        config.outbuf = out_data;
        config.outbuf_size = out_size;

        ESP_LOGI(TAG, "Decoding JPEG image with ROM decoder...");

        ret = esp_jpeg_decode(&config, &out_info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to decode JPEG image: %s", esp_err_to_name(ret));
            heap_caps_free(out_data);
            return nullptr;
        }

        ESP_LOGI(TAG, "JPEG image decoded successfully, size: %d bytes", (int)out_size);

        auto image = std::make_unique<LvglAllocatedImage>(
            out_data, out_size,
            out_info.width, out_info.height,
            out_info.width * 2,
            LV_COLOR_FORMAT_RGB565);

        return image;
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Error fetching image: %s", e.what());
        return nullptr;
    } catch (...) {
        ESP_LOGE(TAG, "Unknown error fetching image");
        return nullptr;
    }
}
