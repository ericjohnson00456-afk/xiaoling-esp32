#ifndef CAMERA_H
#define CAMERA_H

#include <cstdint>
#include <string>

class Camera {
public:
    // 版型即使探测硬件或配置视频流失败，也可能已经创建摄像头对象；
    // 具体实现应重写此接口，返回真实的运行时可用状态。
    virtual bool IsAvailable() const { return true; }
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    // 采集并保留完整帧，仅将显示预览缩放到指定尺寸。
    virtual bool CaptureWithPreviewSize(uint16_t preview_width,
                                        uint16_t preview_height) {
        (void)preview_width;
        (void)preview_height;
        return Capture();
    }
    virtual std::string EncodeJpegBase64(uint8_t quality = 80) {
        (void)quality;
        return {};
    }
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual std::string Explain(const std::string& question) = 0;
};

#endif // CAMERA_H
