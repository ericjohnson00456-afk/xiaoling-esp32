#pragma once
#include "sdkconfig.h"

#ifndef CONFIG_IDF_TARGET_ESP32
#include <lvgl.h>
#include <thread>
#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "image_to_jpeg.h"
#include "esp_cam_sensor_types.h"
#include "esp_video_init.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class Esp32Camera : public Camera {
private:
    struct FrameBuffer {
        uint8_t *data = nullptr;
        size_t len = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        v4l2_pix_fmt_t format = 0;
    } frame_;
    v4l2_pix_fmt_t sensor_format_ = 0;
#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    uint16_t sensor_width_ = 0;
    uint16_t sensor_height_ = 0;
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    int video_fd_ = -1;
    bool streaming_on_ = false;
    struct MmapBuffer { void *start = nullptr; size_t length = 0; };
    std::vector<MmapBuffer> mmap_buffers_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;

public:
    Esp32Camera(const esp_video_init_config_t& config,
                const esp_cam_sensor_format_t* custom_sensor_format = nullptr);
    ~Esp32Camera();

    virtual bool IsAvailable() const override;
    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    virtual bool CaptureWithPreviewSize(uint16_t preview_width,
                                        uint16_t preview_height) override;
    virtual std::string EncodeJpegBase64(uint8_t quality = 80) override;
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question);
};

#endif // ndef CONFIG_IDF_TARGET_ESP32
