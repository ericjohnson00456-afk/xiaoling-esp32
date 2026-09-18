#pragma once

#include "ui/tools/image/lvgl_image.h"

#include <lvgl.h>

#include <memory>
#include <string>

class Display;
class LvglDisplay;

class QrCodeView {
public:
    explicit QrCodeView(Display* display);
    ~QrCodeView();

    void Show();
    void Hide();
    void ShowQrCode(std::unique_ptr<LvglImage> image, const std::string& message);

private:
    void CreateUi();
    void DestroyUi();

    Display* display_ = nullptr;
    LvglDisplay* lvgl_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* image_widget_ = nullptr;
    lv_obj_t* message_label_ = nullptr;
    std::unique_ptr<LvglImage> image_cached_;
};
