#pragma once

#include "sd_card/sd_music_types.h"
#include "ui/nav_stack.h"

#include <lvgl.h>

#include <memory>

class SdMusicSyncView;

class SdMusicSyncPresenter {
public:
    static SdMusicSyncPresenter& GetInstance();
    static const UiNavScreen& Screen();

    void SetState(const SdMusicSyncState& state);
    void Dismiss();

private:
    static int Open(const UiNavScreen*, void** data);
    static int Show(const UiNavScreen*, void* data);
    static int Pause(const UiNavScreen*, void* data);
    static int Resume(const UiNavScreen*, void* data);
    static int Close(const UiNavScreen*, void* data);
    static void AutoReturnTimer(lv_timer_t* timer);
    void ConfigureAutoReturn();

    std::unique_ptr<SdMusicSyncView> view_;
    SdMusicSyncState state_;
    lv_timer_t* auto_return_timer_ = nullptr;
};
