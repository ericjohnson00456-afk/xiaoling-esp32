#include "qrcode_view.h"

#include "display/display.h"
#include "display/lvgl_display.h"
#include "ui/themes/lvgl_theme.h"

QrCodeView::QrCodeView(Display* display) : display_(display) {
    lvgl_ = dynamic_cast<LvglDisplay*>(display_);
    if (lvgl_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            CreateUi();
        }
    }
}

QrCodeView::~QrCodeView() {
    if (lvgl_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            DestroyUi();
        }
    }
}

void QrCodeView::CreateUi() {
    auto* theme = dynamic_cast<LvglTheme*>(display_->GetTheme());
    auto* screen = lv_display_get_screen_active(lvgl_->GetLvglDisplay());
    if (theme == nullptr || screen == nullptr) {
        return;
    }

    root_ = lv_obj_create(screen);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, theme->spacing(4), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, theme->background_color(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);

    image_widget_ = lv_image_create(root_);
    lv_obj_set_width(image_widget_, LV_HOR_RES);
    lv_obj_set_flex_grow(image_widget_, 1);
    lv_image_set_align(image_widget_, LV_IMAGE_ALIGN_CONTAIN);

    message_label_ = lv_label_create(root_);
    lv_obj_set_width(message_label_, LV_HOR_RES);
    lv_obj_set_style_pad_hor(message_label_, theme->spacing(8), 0);
    lv_obj_set_style_text_align(message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(message_label_, theme->text_color(), 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void QrCodeView::DestroyUi() {
    image_cached_.reset();
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    image_widget_ = nullptr;
    message_label_ = nullptr;
}

void QrCodeView::Show() {
    if (lvgl_ != nullptr && root_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void QrCodeView::Hide() {
    if (lvgl_ != nullptr && root_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            if (image_widget_ != nullptr) {
                lv_image_set_src(image_widget_, nullptr);
            }
            image_cached_.reset();
        }
    } else {
#ifdef CONFIG_LSPLATFORM
        if (display_ != nullptr) {
            display_->DismissActivation();
        }
#endif
    }
}

void QrCodeView::ShowQrCode(std::unique_ptr<LvglImage> image, const std::string& message) {
    if (image == nullptr) {
        return;
    }
    if (lvgl_ == nullptr || image_widget_ == nullptr || message_label_ == nullptr) {
#ifdef CONFIG_LSPLATFORM
        if (display_ != nullptr) {
            display_->ShowActivation(std::move(image), message);
        }
#else
        (void)message;
#endif
        return;
    }

    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }
    image_cached_ = std::move(image);
    lv_image_set_src(image_widget_, image_cached_->image_dsc());
    lv_label_set_text(message_label_, message.c_str());
}
