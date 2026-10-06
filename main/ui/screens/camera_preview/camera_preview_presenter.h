#pragma once

#include "ui/nav_stack.h"
#include "ui/ui.h"

#include <memory>
#include <string>

class CameraPreviewView;
class LvglImage;

// 预览页 Presenter：保存页面状态，并在导航生命周期内向 View 转交帧所有权。
class CameraPreviewPresenter {
  public:
    static CameraPreviewPresenter& GetInstance();
    static const UiNavScreen& Screen();

    void SetFrame(std::unique_ptr<LvglImage> image);
    std::unique_ptr<LvglImage> TakeFrame();
    void ClearFrame();
    void SetState(Ui::CameraPreviewPhase phase, std::string detail);

  private:
    static int Open(const UiNavScreen* screen, void** data);
    static int Show(const UiNavScreen* screen, void* data);
    static int Pause(const UiNavScreen* screen, void* data);
    static int Resume(const UiNavScreen* screen, void* data);
    static int Close(const UiNavScreen* screen, void* data);

    std::unique_ptr<CameraPreviewView> view_;
    Ui::CameraPreviewPhase phase_ = Ui::CameraPreviewPhase::Preview;
    std::string detail_;
};
