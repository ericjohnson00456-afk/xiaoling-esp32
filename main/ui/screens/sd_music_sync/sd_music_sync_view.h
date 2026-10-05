#pragma once

#include "sd_card/sd_music_types.h"

#include <lvgl.h>

class Display;

class SdMusicSyncView {
public:
    explicit SdMusicSyncView(Display* display);
    ~SdMusicSyncView();

    void Show();
    void Hide();
    void SetState(const SdMusicSyncState& state);

private:
    void CreateUi();
    void DestroyUi();

    Display* display_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* detail_ = nullptr;
    lv_obj_t* progress_bar_ = nullptr;
    lv_obj_t* progress_ = nullptr;
    lv_obj_t* note_ = nullptr;
};
