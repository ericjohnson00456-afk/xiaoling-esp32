#include "toast.h"

#include "display/display.h"
#include "display/lvgl_display.h"
#include "ui/themes/lvgl_theme.h"
#include "ui/ui_manager.h"

Toast& Toast::GetInstance() {
    static Toast instance;
    return instance;
}

bool Toast::EnsureCreated() {
    if (container_ != nullptr) return true;

    auto* display = UiManager::GetInstance().display();
    auto* lvgl = UiManager::GetInstance().lvgl_display();
    if (display == nullptr || lvgl == nullptr || lvgl->GetLvglDisplay() == nullptr) return false;

    auto* top_layer = lv_display_get_layer_top(lvgl->GetLvglDisplay());
    if (top_layer == nullptr) return false;

    container_ = lv_obj_create(top_layer);
    lv_obj_set_width(container_, LV_HOR_RES * 84 / 100);
    lv_obj_set_height(container_, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_radius(container_, 10, 0);
    lv_obj_set_style_pad_hor(container_, 12, 0);
    lv_obj_set_style_pad_ver(container_, 8, 0);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(container_, LV_ALIGN_CENTER, 0, 0);

    label_ = lv_label_create(container_);
    lv_obj_set_width(label_, LV_PCT(100));
    lv_label_set_long_mode(label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_, lv_color_white(), 0);
    if (auto* theme = dynamic_cast<LvglTheme*>(display->GetTheme());
        theme != nullptr && theme->text_font() != nullptr) {
        lv_obj_set_style_text_font(label_, theme->text_font()->font(), 0);
    }
    lv_label_set_text(label_, "");
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void Toast::Show(const std::string& text, int duration_ms) {
    if (text.empty()) return;

    auto* display = UiManager::GetInstance().display();
    if (display == nullptr) return;
    DisplayLockGuard lock(display);
    if (!lock.locked() || !EnsureCreated()) return;

    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    lv_label_set_text(label_, text.c_str());
    lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);

    timer_ = lv_timer_create(&Toast::TimerCallback,
                             duration_ms > 0 ? duration_ms : 1, this);
    if (timer_ != nullptr) lv_timer_set_repeat_count(timer_, 1);
}

void Toast::Dismiss() {
    auto* display = UiManager::GetInstance().display();
    if (display == nullptr) return;
    DisplayLockGuard lock(display);
    if (!lock.locked()) return;

    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (container_ != nullptr) lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
}

void Toast::TimerCallback(lv_timer_t* timer) {
    auto* toast = timer != nullptr
                      ? static_cast<Toast*>(lv_timer_get_user_data(timer))
                      : nullptr;
    if (toast == nullptr) return;
    if (toast->container_ != nullptr) {
        lv_obj_add_flag(toast->container_, LV_OBJ_FLAG_HIDDEN);
    }
    // LVGL auto-deletes one-shot timers after this callback returns.
    toast->timer_ = nullptr;
}
