#include "alarm_ring_view.h"

#include "display/display.h"
#include "display/lvgl_display.h"
#include "ui/themes/lvgl_theme.h"

#include <font_awesome.h>
#include <lvgl.h>

#include <algorithm>

namespace {

constexpr int kReminderMaxWidthPercent = 82;
constexpr int kReminderVerticalPadding = 12;

void UpdateReminderLayout(lv_obj_t* label, const char* text) {
    if (label == nullptr || text == nullptr || text[0] == '\0') return;

    const auto* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (font == nullptr) return;

    lv_point_t text_size{};
    const int32_t letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    const int32_t line_space = lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    lv_text_get_size(&text_size, text, font, letter_space, line_space,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    const int32_t max_width = LV_HOR_RES * kReminderMaxWidthPercent / 100;
    const int32_t target_width = std::min<int32_t>(text_size.x, max_width);
    lv_label_set_long_mode(label, text_size.x > max_width
                                      ? LV_LABEL_LONG_SCROLL_CIRCULAR
                                      : LV_LABEL_LONG_CLIP);
    lv_obj_set_size(label, std::max<int32_t>(target_width, 1),
                    font->line_height + kReminderVerticalPadding);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

}  // namespace

AlarmRingView::AlarmRingView(Display* display) : display_(display) {
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

    icon_ = lv_label_create(root_);
    lv_label_set_text(icon_, FONT_AWESOME_BELL);
    if (icon_font != nullptr) lv_obj_set_style_text_font(icon_, icon_font, 0);
    lv_obj_set_style_text_color(icon_, theme->system_text_color(), 0);
    lv_obj_align(icon_, LV_ALIGN_TOP_MID, 0, theme->spacing(3));

    time_ = lv_label_create(root_);
    lv_label_set_text(time_, "--:--");
    lv_obj_set_style_text_color(time_, theme->system_text_color(), 0);
    lv_obj_align(time_, LV_ALIGN_TOP_MID, 0, theme->spacing(21));
    date_ = lv_label_create(root_);
    lv_label_set_text(date_, "");
    lv_obj_set_style_text_color(date_, theme->text_color(), 0);
    lv_obj_align(date_, LV_ALIGN_CENTER, 0, -theme->spacing(5));
    text_ = lv_label_create(root_);
    lv_obj_set_size(text_, 1, font != nullptr
                                  ? font->line_height + kReminderVerticalPadding
                                  : 28);
    lv_label_set_long_mode(text_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(text_, theme->text_color(), 0);
    lv_obj_align(text_, LV_ALIGN_CENTER, 0, theme->spacing(10));
    hint_ = lv_label_create(root_);
    lv_obj_set_width(hint_, LV_PCT(95));
    lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint_, theme->text_color(), 0);
    lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -theme->spacing(4));
    blink_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto* icon = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
        if (icon != nullptr) {
            if (lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        }
    }, 500, icon_);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

AlarmRingView::~AlarmRingView() {
    if (root_ == nullptr) return;
    DisplayLockGuard lock(display_);
    if (!lock.locked()) return;
    if (blink_timer_ != nullptr) lv_timer_delete(blink_timer_);
    lv_obj_delete(root_);
}

void AlarmRingView::Show() { if (root_ != nullptr) lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN); }
void AlarmRingView::Hide() { if (root_ != nullptr) lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN); }

void AlarmRingView::SetContent(const std::string& time, const std::string& date,
                               const std::string& text, const std::string& hint) {
    if (root_ == nullptr) return;
    DisplayLockGuard lock(display_);
    if (!lock.locked()) return;
    lv_label_set_text(time_, time.c_str());
    lv_label_set_text(date_, date.c_str());
    lv_label_set_text(hint_, hint.c_str());
    if (text.empty()) {
        lv_label_set_text(text_, "");
        lv_obj_add_flag(text_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(text_, text.c_str());
        UpdateReminderLayout(text_, text.c_str());
        auto* theme = dynamic_cast<LvglTheme*>(display_->GetTheme());
        lv_obj_align(text_, LV_ALIGN_CENTER, 0,
                     theme != nullptr ? theme->spacing(10) : 20);
        lv_obj_remove_flag(text_, LV_OBJ_FLAG_HIDDEN);
    }
}
