#include "sd_music_sync_presenter.h"

#include "sd_music_sync_view.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"

namespace {
constexpr uint32_t kResultHoldMs = 5000;
constexpr uint32_t kWarningHoldMs = 10000;
}

SdMusicSyncPresenter& SdMusicSyncPresenter::GetInstance() {
    static SdMusicSyncPresenter presenter;
    return presenter;
}

const UiNavScreen& SdMusicSyncPresenter::Screen() {
    static const UiNavScreen screen = {
        .unique_id = UI_PAGE_SD_MUSIC_SYNC,
        .open = &SdMusicSyncPresenter::Open,
        .show = &SdMusicSyncPresenter::Show,
        .pause = &SdMusicSyncPresenter::Pause,
        .resume = &SdMusicSyncPresenter::Resume,
        .close = &SdMusicSyncPresenter::Close,
    };
    return screen;
}

int SdMusicSyncPresenter::Open(const UiNavScreen*, void** data) {
    auto& presenter = GetInstance();
    presenter.view_ = std::make_unique<SdMusicSyncView>(
        UiManager::GetInstance().display());
    presenter.auto_return_timer_ = lv_timer_create(
        AutoReturnTimer, kResultHoldMs, &presenter);
    if (presenter.auto_return_timer_ != nullptr) {
        lv_timer_set_repeat_count(presenter.auto_return_timer_, 1);
        lv_timer_pause(presenter.auto_return_timer_);
    }
    *data = &presenter;
    return 0;
}

int SdMusicSyncPresenter::Show(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<SdMusicSyncPresenter*>(data);
    if (presenter == nullptr || presenter->view_ == nullptr) return -1;
    presenter->view_->Show();
    presenter->view_->SetState(presenter->state_);
    presenter->ConfigureAutoReturn();
    return 0;
}

int SdMusicSyncPresenter::Pause(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<SdMusicSyncPresenter*>(data);
    if (presenter != nullptr && presenter->view_ != nullptr) presenter->view_->Hide();
    return 0;
}

int SdMusicSyncPresenter::Resume(const UiNavScreen* screen, void* data) {
    return Show(screen, data);
}

int SdMusicSyncPresenter::Close(const UiNavScreen*, void* data) {
    auto* presenter = static_cast<SdMusicSyncPresenter*>(data);
    if (presenter == nullptr) return 0;
    if (presenter->auto_return_timer_ != nullptr) {
        lv_timer_delete(presenter->auto_return_timer_);
        presenter->auto_return_timer_ = nullptr;
    }
    presenter->view_.reset();
    return 0;
}

void SdMusicSyncPresenter::SetState(const SdMusicSyncState& state) {
    state_ = state;
    auto& manager = UiManager::GetInstance();
    if (state.phase == SdMusicSyncPhase::kIdle) {
        Dismiss();
        return;
    }
    if (manager.TopScreenId() != UI_PAGE_SD_MUSIC_SYNC) {
        manager.NavigateTo(UI_PAGE_SD_MUSIC_SYNC);
    }
    if (view_ != nullptr) view_->SetState(state_);
    ConfigureAutoReturn();
}

void SdMusicSyncPresenter::Dismiss() {
    auto& manager = UiManager::GetInstance();
    if (manager.TopScreenId() == UI_PAGE_SD_MUSIC_SYNC) manager.NavigateHome();
}

void SdMusicSyncPresenter::ConfigureAutoReturn() {
    if (auto_return_timer_ == nullptr) return;
    if (state_.phase == SdMusicSyncPhase::kSucceeded ||
        state_.phase == SdMusicSyncPhase::kFailed) {
        const uint32_t hold = state_.skipped_count != 0
                                  ? kWarningHoldMs
                                  : kResultHoldMs;
        lv_timer_set_period(auto_return_timer_, hold);
        lv_timer_reset(auto_return_timer_);
        lv_timer_resume(auto_return_timer_);
    } else {
        lv_timer_pause(auto_return_timer_);
    }
}

void SdMusicSyncPresenter::AutoReturnTimer(lv_timer_t* timer) {
    auto* presenter = static_cast<SdMusicSyncPresenter*>(lv_timer_get_user_data(timer));
    if (presenter != nullptr) presenter->Dismiss();
}
