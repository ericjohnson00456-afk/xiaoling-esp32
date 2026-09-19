#include "upgrade_presenter.h"

#include "display/display.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"
#include "upgrade_view.h"

#include <utility>

UpgradePresenter& UpgradePresenter::GetInstance() {
    static UpgradePresenter instance;
    return instance;
}

const UiNavScreen& UpgradePresenter::Screen() {
    static const UiNavScreen screen = {
        .unique_id = UI_PAGE_UPGRADE,
        .open = &UpgradePresenter::Open,
        .show = &UpgradePresenter::Show,
        .pause = &UpgradePresenter::Pause,
        .resume = &UpgradePresenter::Resume,
        .close = &UpgradePresenter::Close,
    };
    return screen;
}

int UpgradePresenter::Open(const UiNavScreen*, void** data) {
    auto& presenter = GetInstance();
    presenter.view_ = std::make_unique<UpgradeView>(UiManager::GetInstance().display());
    *data = &presenter;
    return 0;
}

int UpgradePresenter::Show(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<UpgradePresenter*>(data);
    if (presenter == nullptr || presenter->view_ == nullptr) return -1;
    presenter->view_->Show();
    presenter->view_->SetProgress(presenter->target_, presenter->progress_, presenter->speed_);
    return 0;
}

int UpgradePresenter::Pause(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<UpgradePresenter*>(data);
    if (presenter != nullptr && presenter->view_ != nullptr) presenter->view_->Hide();
    return 0;
}

int UpgradePresenter::Resume(const UiNavScreen* screen, void* data) { return Show(screen, data); }

int UpgradePresenter::Close(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<UpgradePresenter*>(data);
    if (presenter != nullptr) presenter->view_.reset();
    return 0;
}

void UpgradePresenter::SetProgress(std::string target, int progress, size_t speed) {
    target_ = std::move(target);
    progress_ = progress;
    speed_ = speed;
    if (view_ != nullptr) view_->SetProgress(target_, progress_, speed_);
}

void UpgradePresenter::SetResult(std::string message) {
    target_ = std::move(message);
    progress_ = 100;
    speed_ = 0;
    if (view_ != nullptr) view_->SetResult(target_);
}
