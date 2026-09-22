#include "alarm_success_view.h"

#include "display/display.h"
#include "display/lvgl_display.h"
#include "ui/themes/lvgl_theme.h"

#include <font_awesome.h>
#include <lvgl.h>

AlarmSuccessView::AlarmSuccessView(Display* display) : display_(display) {
    auto* lvgl = dynamic_cast<LvglDisplay*>(display_);
    auto* theme = display_ != nullptr ? dynamic_cast<LvglTheme*>(display_->GetTheme()) : nullptr;
    if (lvgl == nullptr || theme == nullptr) return;
    auto* font = theme->text_font() != nullptr ? theme->text_font()->font() : nullptr;
    auto* icon_font = theme->large_icon_font() != nullptr ? theme->large_icon_font()->font() : nullptr;
    DisplayLockGuard lock(display_);
    if (!lock.locked()) return;
    root_ = lv_obj_create(lv_display_get_screen_active(lvgl->GetLvglDisplay()));
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, theme->background_color(), 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    if (font != nullptr) lv_obj_set_style_text_font(root_, font, 0);

    auto* icon = lv_label_create(root_);
    lv_label_set_text(icon, FONT_AWESOME_ALARM_CLOCK);
    if (icon_font != nullptr) lv_obj_set_style_text_font(icon, icon_font, 0);
    lv_obj_set_style_text_color(icon, theme->system_text_color(), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, theme->spacing(5));

    auto* title = lv_label_create(root_);
    lv_label_set_text(title, "闹钟设置成功");
    lv_obj_set_style_text_color(title, theme->text_color(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -theme->spacing(10));

    time_ = lv_label_create(root_);
    lv_label_set_text(time_, "--:--");
    lv_obj_set_style_text_color(time_, theme->system_text_color(), 0);
    lv_obj_align(time_, LV_ALIGN_CENTER, 0, theme->spacing(5));

    date_ = lv_label_create(root_);
    lv_label_set_text(date_, "");
    lv_obj_set_style_text_color(date_, theme->text_color(), 0);
    lv_obj_align(date_, LV_ALIGN_BOTTOM_MID, 0, -theme->spacing(6));
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

AlarmSuccessView::~AlarmSuccessView() {
    if (root_ == nullptr) return;
    DisplayLockGuard lock(display_);
    if (lock.locked()) lv_obj_delete(root_);
}

void AlarmSuccessView::Show() { if (root_ != nullptr) lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN); }
void AlarmSuccessView::Hide() { if (root_ != nullptr) lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN); }

void AlarmSuccessView::SetContent(const std::string& time, const std::string& date) {
    if (root_ == nullptr) return;
    DisplayLockGuard lock(display_);
    if (!lock.locked()) return;
    lv_label_set_text(time_, time.c_str());
    lv_label_set_text(date_, date.c_str());
}
