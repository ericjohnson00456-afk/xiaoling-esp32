#include "camera_preview_view.h"

#include "display/display.h"
#include "display/lvgl_display.h"
#include "ui/themes/lvgl_theme.h"

namespace {

constexpr lv_coord_t kPreviewImageWidth = 160;
constexpr lv_coord_t kPreviewImageHeight = 120;

void MakeOverlayBand(lv_obj_t* object, lv_coord_t height, lv_align_t align) {
    lv_obj_set_size(object, LV_HOR_RES, height);
    lv_obj_align(object, align, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x101214), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_80, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

}  // 匿名命名空间

CameraPreviewView::CameraPreviewView(Display* display) : display_(display) {
    if (dynamic_cast<LvglDisplay*>(display_) != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            CreateUi();
        }
    }
}

CameraPreviewView::~CameraPreviewView() {
    if (root_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            DestroyUi();
        }
    }
}

void CameraPreviewView::CreateUi() {
    auto* lvgl = dynamic_cast<LvglDisplay*>(display_);
    auto* theme = display_ != nullptr
                      ? dynamic_cast<LvglTheme*>(display_->GetTheme())
                      : nullptr;
    if (lvgl == nullptr || theme == nullptr) {
        return;
    }
    const auto* font =
        theme->text_font() != nullptr ? theme->text_font()->font() : nullptr;

    root_ = lv_obj_create(lv_display_get_screen_active(lvgl->GetLvglDisplay()));
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    image_ = lv_image_create(root_);
    lv_obj_set_size(image_, kPreviewImageWidth, kPreviewImageHeight);
    lv_obj_center(image_);
    lv_image_set_align(image_, LV_IMAGE_ALIGN_CONTAIN);

    top_band_ = lv_obj_create(root_);
    MakeOverlayBand(top_band_, 38, LV_ALIGN_TOP_MID);
    lv_obj_set_style_bg_opa(top_band_, LV_OPA_TRANSP, 0);
    title_ = lv_label_create(top_band_);
    lv_obj_set_width(title_, LV_PCT(94));
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title_, lv_color_white(), 0);
    if (font != nullptr) {
        lv_obj_set_style_text_font(title_, font, 0);
    }
    lv_obj_center(title_);

    center_band_ = lv_obj_create(root_);
    MakeOverlayBand(center_band_, 72, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(center_band_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center_band_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(center_band_, theme->spacing(5), 0);
    spinner_ = lv_arc_create(center_band_);
    lv_obj_set_size(spinner_, 28, 28);
    lv_arc_set_bg_angles(spinner_, 0, 360);
    lv_arc_set_angles(spinner_, 25, 295);
    lv_obj_remove_style(spinner_, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(spinner_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(spinner_, lv_color_hex(0x56616A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner_, lv_color_hex(0x42C7A5),
                               LV_PART_INDICATOR);
    detail_ = lv_label_create(center_band_);
    lv_obj_set_width(detail_, LV_PCT(72));
    lv_label_set_long_mode(detail_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(detail_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(detail_, lv_color_white(), 0);
    if (font != nullptr) {
        lv_obj_set_style_text_font(detail_, font, 0);
    }

    bottom_band_ = lv_obj_create(root_);
    MakeOverlayBand(bottom_band_, 48, LV_ALIGN_BOTTOM_MID);
    hint_ = lv_label_create(bottom_band_);
    lv_obj_set_width(hint_, LV_PCT(94));
    lv_label_set_long_mode(hint_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint_, lv_color_hex(0xD8DEE4), 0);
    if (font != nullptr) {
        lv_obj_set_style_text_font(hint_, font, 0);
    }
    lv_obj_center(hint_);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void CameraPreviewView::DestroyUi() {
    frame_.reset();
    lv_obj_delete(root_);
    root_ = image_ = top_band_ = title_ = center_band_ = spinner_ = detail_ =
        bottom_band_ = hint_ = nullptr;
}

void CameraPreviewView::Show() {
    if (root_ != nullptr) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CameraPreviewView::Hide() {
    if (root_ != nullptr) {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CameraPreviewView::SetFrame(std::unique_ptr<LvglImage> image) {
    if (image_ == nullptr || image == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }
    frame_ = std::move(image);
    lv_image_set_src(image_, frame_->image_dsc());
}

std::unique_ptr<LvglImage> CameraPreviewView::TakeFrame() {
    if (image_ == nullptr) {
        return std::move(frame_);
    }
    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return nullptr;
    }
    lv_image_set_src(image_, nullptr);
    return std::move(frame_);
}

void CameraPreviewView::ClearFrame() {
    if (image_ == nullptr) {
        frame_.reset();
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }
    lv_image_set_src(image_, nullptr);
    frame_.reset();
}

void CameraPreviewView::SetState(Ui::CameraPreviewPhase phase,
                                 const std::string& detail) {
    if (root_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }

    lv_obj_add_flag(center_band_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(spinner_, LV_OBJ_FLAG_HIDDEN);
    switch (phase) {
        case Ui::CameraPreviewPhase::Preview:
            lv_label_set_text(title_, "拍照预览");
            lv_label_set_text(hint_, "单击拍照  长按退出");
            break;
        case Ui::CameraPreviewPhase::Processing:
            lv_label_set_text(title_, "拍照完成");
            lv_label_set_text(detail_, detail.empty() ? "正在识别照片..."
                                                      : detail.c_str());
            lv_label_set_text(hint_, "长按可退出");
            lv_obj_remove_flag(center_band_, LV_OBJ_FLAG_HIDDEN);
            break;
        case Ui::CameraPreviewPhase::Speaking:
            lv_label_set_text(title_, "识别结果");
            lv_label_set_text(detail_,
                              detail.empty() ? "正在播报..." : detail.c_str());
            lv_label_set_text(hint_, "播完自动退出  长按退出");
            lv_obj_add_flag(spinner_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(center_band_, LV_OBJ_FLAG_HIDDEN);
            break;
        case Ui::CameraPreviewPhase::Error:
            lv_label_set_text(title_, "识别失败");
            lv_label_set_text(detail_,
                              detail.empty() ? "请稍后重试" : detail.c_str());
            lv_label_set_text(hint_, "即将退出");
            lv_obj_add_flag(spinner_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(center_band_, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}
