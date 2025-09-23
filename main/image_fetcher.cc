#include "image_fetcher.h"

#include <cstring>

#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_jpeg_dec.h>

#include "board.h"
#include "network_interface.h"

#define TAG "ImageFetcher"

bool ImageFetcher::Fetch(const std::string& url, lv_img_dsc_t* into, int timeout_ms) {
    int ret;

    ESP_LOGI(TAG, "Fetching image from %s", url.c_str());

    try {
        auto network = board_->GetNetwork();
        if (!network) {
            ESP_LOGE(TAG, "Failed to get network instance");
            return false;
        }

        auto http = network->CreateHttp();
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            return false;
        }

        http->SetTimeout(timeout_ms);

        auto app_desc = esp_app_get_description();
        auto user_agent = std::string(BOARD_NAME "/") + app_desc->version;
        http->SetHeader("User-Agent", user_agent);

        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "Failed to connect to %s", url.c_str());
            return false;
        }

        int status_code = http->GetStatusCode();
        if (status_code != 200) {
            ESP_LOGE(TAG, "Failed to fetch image, status code: %d", status_code);
            http->Close();
            return false;
        }

        std::string image_data = http->ReadAll();
        http->Close();

        if (image_data.empty()) {
            ESP_LOGE(TAG, "No image data received");
            return false;
        }

        ESP_LOGI(TAG, "Downloaded image data: %u bytes", image_data.size());

        if (image_data.size() > 5 * 1024 * 1024) {
            ESP_LOGE(TAG, "Image data too large");
            return false;
        }

        if (image_data.size() < 10 ||
            image_data[0] != 0xFF || image_data[1] != 0xD8) {
            ESP_LOGE(TAG, "Invalid JPEG header");
            return false;
        }

        jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
        config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

        jpeg_dec_handle_t jpeg_dec = NULL;
        ret = jpeg_dec_open(&config, &jpeg_dec);
        if (ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open JPEG decoder, ret=%d", ret);
            return false;
        }

        jpeg_dec_io_t jpeg_io = {0};
        jpeg_io.inbuf = (uint8_t *)image_data.data();
        jpeg_io.inbuf_len = image_data.size();

        jpeg_dec_header_info_t out_info = {0};

        ret = jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info);
        if (ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to get JPEG header info, ret=%d", ret);
            jpeg_dec_close(jpeg_dec);
            return false;
        }

        ESP_LOGI(TAG, "JPEG header info: width=%d, height=%d", out_info.width, out_info.height);

        if (out_info.width == 0 || out_info.height == 0 ||
            out_info.width > 2048 || out_info.height > 2048) {
            ESP_LOGE(TAG, "Invalid JPEG dimensions: %dx%d", out_info.width, out_info.height);
            jpeg_dec_close(jpeg_dec);
            return false;
        }

        size_t out_size = LV_DRAW_BUF_SIZE(out_info.width, out_info.height, LV_COLOR_FORMAT_RGB565);
        if (out_size > 2 * 1024 * 1024) {
            ESP_LOGE(TAG, "Decoded image too large");
            jpeg_dec_close(jpeg_dec);
            return false;
        }

        uint8_t* out_buf = (uint8_t*)jpeg_calloc_align(out_size, 16);
        if (!out_buf) {
            ESP_LOGE(TAG, "Failed to allocate memory for decoded image, size: %d bytes", (int)out_size);
            jpeg_dec_close(jpeg_dec);
            return false;
        }
 
        jpeg_io.inbuf += jpeg_io.inbuf_len - jpeg_io.inbuf_remain;
        jpeg_io.inbuf_len = jpeg_io.inbuf_remain;
        jpeg_io.outbuf = out_buf;

        ESP_LOGI(TAG, "Decoding JPEG image...");

        ret = jpeg_dec_process(jpeg_dec, &jpeg_io);
        if (ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to decode JPEG image, ret=%d", ret);
            jpeg_free_align(out_buf);
            jpeg_dec_close(jpeg_dec);
            return false;
        }

        ESP_LOGI(TAG, "JPEG image decoded successfully, size: %d bytes", (int)out_size);

        if (rgb_buffer_) {
            jpeg_free_align(rgb_buffer_);
            ESP_LOGI(TAG, "Freed previous RGB buffer at %p", rgb_buffer_);
            rgb_buffer_ = nullptr;
        }

        memset(into, 0, sizeof(lv_img_dsc_t));
        into->header.magic = LV_IMAGE_HEADER_MAGIC;
        into->header.cf = LV_COLOR_FORMAT_RGB565;
        into->header.w = out_info.width;
        into->header.h = out_info.height;
        into->header.stride = LV_DRAW_BUF_STRIDE(out_info.width, LV_COLOR_FORMAT_RGB565);
        into->data_size = out_size;
        into->data = out_buf;

        rgb_buffer_ = out_buf; 
        jpeg_dec_close(jpeg_dec);

        return true;
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Error fetching image: %s", e.what());
        return false;
    } catch (...) {
        ESP_LOGE(TAG, "Unknown error fetching image");
        return false;
    }
}

ImageFetcher::~ImageFetcher() {
    if (rgb_buffer_) {
        jpeg_free_align(rgb_buffer_);
        rgb_buffer_ = nullptr;
    }
}
