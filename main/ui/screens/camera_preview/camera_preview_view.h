#pragma once

#include "ui/ui.h"
#include "ui/utils/image/lvgl_image.h"

#include <lvgl.h>

#include <memory>
#include <string>

class Display;

// 预览页 View：只负责 LVGL 控件、160x120 实时帧和页面状态文案。
class CameraPreviewView {
  public:
    explicit CameraPreviewView(Display* display);
    ~CameraPreviewView();

    void Show();
    void Hide();
    void SetFrame(std::unique_ptr<LvglImage> image);
    std::unique_ptr<LvglImage> TakeFrame();
    void ClearFrame();
    void SetState(Ui::CameraPreviewPhase phase, const std::string& detail);

  private:
    void CreateUi();
    void DestroyUi();

    Display* display_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_obj_t* top_band_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* center_band_ = nullptr;
    lv_obj_t* spinner_ = nullptr;
    lv_obj_t* detail_ = nullptr;
    lv_obj_t* bottom_band_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    std::unique_ptr<LvglImage> frame_;
};
