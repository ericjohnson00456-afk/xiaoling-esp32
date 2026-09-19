#include "upgrade_view.h"

#include "display/display.h"
#include "display/lvgl_display.h"
#include "ui/themes/lvgl_theme.h"

#include <algorithm>
#include <cstdio>

UpgradeView::UpgradeView(Display* display) : display_(display) {
    if (dynamic_cast<LvglDisplay*>(display_) != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) CreateUi();
    }
}

UpgradeView::~UpgradeView() {
    if (root_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) DestroyUi();
    }
}

void UpgradeView::CreateUi() {
    auto* lvgl = dynamic_cast<LvglDisplay*>(display_);
    auto* theme = display_ != nullptr ? dynamic_cast<LvglTheme*>(display_->GetTheme()) : nullptr;
    if (lvgl == nullptr || theme == nullptr) return;

    root_ = lv_obj_create(lv_display_get_screen_active(lvgl->GetLvglDisplay()));
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, theme->spacing(8), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, theme->background_color(), 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    title_ = lv_label_create(root_);
    lv_label_set_text(title_, "OTA");
    lv_obj_set_width(title_, LV_PCT(100));
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title_, theme->text_color(), 0);
    lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, theme->spacing(8));

    target_ = lv_label_create(root_);
    lv_label_set_text(target_, "Checking for updates...");
    lv_obj_set_width(target_, LV_PCT(100));
    lv_obj_set_style_text_align(target_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(target_, theme->text_color(), 0);
    lv_obj_align(target_, LV_ALIGN_CENTER, 0, -theme->spacing(14));

    bar_ = lv_bar_create(root_);
    lv_obj_set_width(bar_, LV_PCT(85));
    lv_obj_set_height(bar_, theme->spacing(5));
    lv_bar_set_range(bar_, 0, 100);
    lv_bar_set_value(bar_, 0, LV_ANIM_OFF);
    lv_obj_align(bar_, LV_ALIGN_CENTER, 0, theme->spacing(4));

    percent_ = lv_label_create(root_);
    lv_label_set_text(percent_, "0%");
    lv_obj_set_width(percent_, LV_PCT(100));
    lv_obj_set_style_text_align(percent_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(percent_, theme->text_color(), 0);
    lv_obj_align(percent_, LV_ALIGN_CENTER, 0, theme->spacing(18));

    speed_ = lv_label_create(root_);
    lv_label_set_text(speed_, "");
    lv_obj_set_width(speed_, LV_PCT(100));
    lv_obj_set_style_text_align(speed_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(speed_, theme->text_color(), 0);
    lv_obj_align(speed_, LV_ALIGN_BOTTOM_MID, 0, -theme->spacing(8));
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void UpgradeView::DestroyUi() {
    if (root_ != nullptr) lv_obj_delete(root_);
    root_ = title_ = target_ = bar_ = percent_ = speed_ = nullptr;
}

void UpgradeView::Show() { if (root_ != nullptr) lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN); }
void UpgradeView::Hide() { if (root_ != nullptr) lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN); }

void UpgradeView::SetProgress(const std::string& target, int progress, size_t speed) {
    if (root_ == nullptr) return;
    const int bounded = std::max(0, std::min(100, progress));
    char text[48];
    lv_label_set_text(target_, target.c_str());
    lv_bar_set_value(bar_, bounded, LV_ANIM_OFF);
    std::snprintf(text, sizeof(text), "%d%%", bounded);
    lv_label_set_text(percent_, text);
    if (speed != 0) {
        std::snprintf(text, sizeof(text), "%u KB/s", static_cast<unsigned>(speed / 1024));
        lv_label_set_text(speed_, text);
    } else {
        lv_label_set_text(speed_, "");
    }
}

void UpgradeView::SetResult(const std::string& message) {
    if (root_ == nullptr) return;
    DisplayLockGuard lock(display_);
    if (!lock.locked()) return;
    lv_label_set_text(target_, message.c_str());
    lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(percent_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(speed_, LV_OBJ_FLAG_HIDDEN);
}
