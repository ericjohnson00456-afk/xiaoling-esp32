#include "sd_music_sync_view.h"

#include "display/display.h"
#include "display/lvgl_display.h"
#include "ui/themes/lvgl_theme.h"

#include <algorithm>
#include <cstdio>

namespace {

lv_obj_t* CreateLabel(lv_obj_t* parent, const lv_font_t* font,
                      lv_color_t color, int y) {
    auto* label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(92));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, color, 0);
    if (font != nullptr) lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

}  // namespace

SdMusicSyncView::SdMusicSyncView(Display* display) : display_(display) {
    if (dynamic_cast<LvglDisplay*>(display_) != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) CreateUi();
    }
}

SdMusicSyncView::~SdMusicSyncView() {
    if (root_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) DestroyUi();
    }
}

void SdMusicSyncView::CreateUi() {
    auto* lvgl = dynamic_cast<LvglDisplay*>(display_);
    auto* theme = display_ != nullptr
                      ? dynamic_cast<LvglTheme*>(display_->GetTheme())
                      : nullptr;
    if (lvgl == nullptr || theme == nullptr) return;
    const auto* font = theme->text_font() != nullptr
                           ? theme->text_font()->font()
                           : nullptr;

    root_ = lv_obj_create(lv_display_get_screen_active(lvgl->GetLvglDisplay()));
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, theme->spacing(6), 0);
    lv_obj_set_style_bg_color(root_, theme->background_color(), 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    title_ = CreateLabel(root_, font, theme->text_color(), 42);
    detail_ = CreateLabel(root_, font, theme->text_color(), 82);
    lv_obj_set_style_text_opa(detail_, LV_OPA_70, 0);

    progress_bar_ = lv_bar_create(root_);
    lv_obj_set_size(progress_bar_, LV_PCT(82), theme->spacing(5));
    lv_bar_set_range(progress_bar_, 0, 1000);
    lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
    lv_obj_align(progress_bar_, LV_ALIGN_TOP_MID, 0, 122);

    progress_ = CreateLabel(root_, font, theme->text_color(), 140);
    note_ = CreateLabel(root_, font, theme->text_color(), 172);
    lv_obj_set_style_text_opa(note_, LV_OPA_70, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void SdMusicSyncView::DestroyUi() {
    lv_obj_delete(root_);
    root_ = title_ = detail_ = progress_bar_ = progress_ = note_ = nullptr;
}

void SdMusicSyncView::Show() {
    if (root_ != nullptr) lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void SdMusicSyncView::Hide() {
    if (root_ != nullptr) lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void SdMusicSyncView::SetState(const SdMusicSyncState& state) {
    if (root_ == nullptr) return;
    DisplayLockGuard lock(display_);
    if (!lock.locked()) return;

    char text[256] = {};
    lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_align(detail_, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_align(progress_bar_, LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_align(progress_, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_align(note_, LV_ALIGN_TOP_MID, 0, 172);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(progress_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(progress_, LV_OBJ_FLAG_HIDDEN);
    switch (state.phase) {
        case SdMusicSyncPhase::kScanning:
            lv_obj_add_flag(progress_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(progress_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(title_, "检测到TF卡插入，扫描中...");
            lv_label_set_text(detail_, "请勿拔卡或关机");
            lv_label_set_text(note_, "");
            break;
        case SdMusicSyncPhase::kUploading: {
            lv_label_set_text(title_, "正在处理TF卡音频文件列表");
            const uint32_t total = std::max(state.total_count,
                                            state.uploaded_count);
            if (total != 0) {
                std::snprintf(text, sizeof(text), "已成功处理 %lu / %lu 个",
                              static_cast<unsigned long>(state.uploaded_count),
                              static_cast<unsigned long>(total));
                lv_bar_set_value(
                    progress_bar_,
                    static_cast<int>(state.uploaded_count * 1000ULL / total),
                    LV_ANIM_OFF);
            } else {
                std::snprintf(text, sizeof(text), "已成功处理 %lu 个",
                              static_cast<unsigned long>(state.uploaded_count));
                lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
            }
            lv_label_set_text(detail_, text);
            lv_label_set_text(progress_, "");
            lv_label_set_text(note_, "请勿拔卡或关机");
            break;
        }
        case SdMusicSyncPhase::kSucceeded:
            lv_obj_add_flag(progress_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(progress_, LV_OBJ_FLAG_HIDDEN);
            if (state.result == SdMusicSyncResult::kDataUnchanged) {
                lv_label_set_text(title_, "检测到TF卡插入，但数据无变化");
                lv_label_set_text(detail_, "");
                lv_label_set_text(note_, "");
            } else {
                lv_label_set_text(title_, "处理完成");
                std::snprintf(text, sizeof(text), "已上报 %lu 个音频文件",
                              static_cast<unsigned long>(state.uploaded_count));
                lv_label_set_text(detail_, text);
                if (state.skipped_count != 0) {
                    lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 24);
                    lv_obj_align(detail_, LV_ALIGN_TOP_MID, 0, 56);
                    lv_obj_align(note_, LV_ALIGN_TOP_MID, 0, 84);
                    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_LEFT, 0);
                    std::snprintf(text, sizeof(text),
                                  "未处理 %lu 个，原因可能有\n"
                                  "1. 文件名超过64字\n"
                                  "2. 文件夹名超过32字\n"
                                  "3. 文件夹层数超过3层\n"
                                  "4. 文件不在audio文件夹内",
                                  static_cast<unsigned long>(state.skipped_count));
                    lv_label_set_text(note_, text);
                } else {
                    lv_label_set_text(note_, "");
                }
            }
            break;
        case SdMusicSyncPhase::kFailed:
            lv_obj_add_flag(progress_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(progress_, LV_OBJ_FLAG_HIDDEN);
            if (state.result == SdMusicSyncResult::kUnsupportedFileSystem) {
                lv_label_set_text(title_, "TF卡仅支持FAT32文件系统");
                lv_label_set_text(detail_, "请重新格式化后插入");
                lv_label_set_text(note_, "");
                break;
            }
            lv_label_set_text(title_, "TF卡文件列表上报失败");
            if (state.http_status_code != 0) {
                std::snprintf(text, sizeof(text), "HTTP响应码：%lu",
                              static_cast<unsigned long>(state.http_status_code));
            } else if (state.http_error_code != 0) {
                std::snprintf(text, sizeof(text), "错误码：%ld",
                              static_cast<long>(state.http_error_code));
            }
            lv_label_set_text(detail_, text);
            lv_label_set_text(note_, "请检查网络后重新插卡");
            break;
        case SdMusicSyncPhase::kIdle:
            break;
    }
}
