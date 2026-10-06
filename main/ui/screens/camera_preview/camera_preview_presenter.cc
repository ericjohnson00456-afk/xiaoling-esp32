#include "camera_preview_presenter.h"

#include "camera_preview_view.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"

CameraPreviewPresenter& CameraPreviewPresenter::GetInstance() {
    static CameraPreviewPresenter presenter;
    return presenter;
}

const UiNavScreen& CameraPreviewPresenter::Screen() {
    static const UiNavScreen screen = {
        .unique_id = UI_PAGE_CAMERA_PREVIEW,
        .open = &CameraPreviewPresenter::Open,
        .show = &CameraPreviewPresenter::Show,
        .pause = &CameraPreviewPresenter::Pause,
        .resume = &CameraPreviewPresenter::Resume,
        .close = &CameraPreviewPresenter::Close,
    };
    return screen;
}

int CameraPreviewPresenter::Open(const UiNavScreen*, void** data) {
    auto& presenter = GetInstance();
    presenter.view_ =
        std::make_unique<CameraPreviewView>(UiManager::GetInstance().display());
    *data = &presenter;
    return 0;
}

int CameraPreviewPresenter::Show(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<CameraPreviewPresenter*>(data);
    if (presenter == nullptr || presenter->view_ == nullptr) {
        return -1;
    }
    presenter->view_->Show();
    presenter->view_->SetState(presenter->phase_, presenter->detail_);
    return 0;
}

int CameraPreviewPresenter::Pause(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<CameraPreviewPresenter*>(data);
    if (presenter != nullptr && presenter->view_ != nullptr) {
        presenter->view_->Hide();
    }
    return 0;
}

int CameraPreviewPresenter::Resume(const UiNavScreen* screen, void* data) {
    return Show(screen, data);
}

int CameraPreviewPresenter::Close(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<CameraPreviewPresenter*>(data);
    if (presenter != nullptr) {
        presenter->view_.reset();
    }
    return 0;
}

void CameraPreviewPresenter::SetFrame(std::unique_ptr<LvglImage> image) {
    if (view_ != nullptr && image != nullptr) {
        view_->SetFrame(std::move(image));
    }
}

std::unique_ptr<LvglImage> CameraPreviewPresenter::TakeFrame() {
    return view_ != nullptr ? view_->TakeFrame() : nullptr;
}

void CameraPreviewPresenter::ClearFrame() {
    if (view_ != nullptr) {
        view_->ClearFrame();
    }
}

void CameraPreviewPresenter::SetState(Ui::CameraPreviewPhase phase,
                                      std::string detail) {
    phase_ = phase;
    detail_ = std::move(detail);
    if (view_ != nullptr) {
        view_->SetState(phase_, detail_);
    }
}
