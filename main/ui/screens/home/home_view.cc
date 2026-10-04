#include "home_view.h"

#include "display/display.h"
#include "resource/resource.h"
#include "ui/ui_manager.h"
#include "display/lvgl_display.h"
#include "ui/themes/lvgl_theme.h"
#include "xiaoling/session/session_core.h"

#include <font_awesome.h>
#include <lvgl.h>

#include <cstring>
#include <algorithm>
#include <map>
#include <utility>

namespace {

#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
const std::map<std::string, std::string> kEmotionAliases = {
    {"neutral", "neutral"}, {"wakeup", "聆听"}, {"speaking", "说话"},
    {"happy", "开心"}, {"laughing", "大笑"}, {"funny", "等待"},
    {"sad", "伤心"}, {"angry", "生气"}, {"crying", "伤心"},
    {"loving", "撒娇"}, {"embarrassed", "开心"}, {"surprised", "开心"},
    {"shocked", "生气"}, {"thinking", "等待"}, {"winking", "开心"},
    {"cool", "开心"}, {"relaxed", "待机"}, {"delicious", "开心"},
    {"kissy", "撒娇"}, {"confident", "开心"}, {"sleepy", "sleepy"},
    {"silly", "等待"}, {"confused", "等待"},
};
#endif

void SetTransparent(lv_obj_t* object) {
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
}

}  // namespace

HomeView::HomeView(Display* display) : display_(display) {
    lvgl_ = dynamic_cast<LvglDisplay*>(display_);
    if (lvgl_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            CreateUi();
        }
    }
}

HomeView::~HomeView() {
    if (lvgl_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            DestroyUi();
        }
    }
}

void HomeView::CreateUi() {
    auto* theme = dynamic_cast<LvglTheme*>(display_->GetTheme());
    auto* screen = lv_display_get_screen_active(lvgl_->GetLvglDisplay());
    if (theme == nullptr || screen == nullptr) {
        return;
    }

    auto* text_font = theme->text_font() != nullptr ? theme->text_font()->font() : nullptr;
    auto* icon_font = theme->icon_font() != nullptr ? theme->icon_font()->font() : nullptr;
    auto* large_font = theme->large_icon_font() != nullptr ? theme->large_icon_font()->font() : nullptr;

    root_ = lv_obj_create(screen);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, theme->background_color(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);

    auto* top_bar = lv_obj_create(root_);
    lv_obj_set_size(top_bar, LV_HOR_RES, LV_SIZE_CONTENT);
    SetTransparent(top_bar);
    lv_obj_set_style_pad_hor(top_bar, theme->spacing(4), 0);
    lv_obj_set_style_pad_ver(top_bar, theme->spacing(2), 0);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    network_label_ = lv_label_create(top_bar);
    lv_label_set_text(network_label_, "");
    if (icon_font != nullptr) {
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }
    lv_obj_set_style_text_color(network_label_, theme->text_color(), 0);

    status_label_ = lv_label_create(root_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 80 / 100);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, theme->text_color(), 0);
    lv_label_set_text(status_label_, "");
    lv_obj_set_style_min_height(status_label_, text_font != nullptr ? text_font->line_height : 0, 0);

    notification_label_ = lv_label_create(root_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 80 / 100);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    content_ = lv_obj_create(root_);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, theme->spacing(4), 0);
    lv_obj_set_style_bg_color(content_, theme->chat_background_color(), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content_, theme->spacing(4), 0);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);

    subtitle_label_ = lv_label_create(root_);
    lv_obj_set_width(subtitle_label_, LV_HOR_RES * 90 / 100);
    // Keep long streamed TTS text inside the lower subtitle area. Three
    // lines fit the 240px panel without covering the bottom of the face.
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
    if (text_font != nullptr) {
        lv_obj_set_style_text_font(subtitle_label_, text_font, 0);
        lv_obj_set_height(subtitle_label_, text_font->line_height * 3);
    }
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(subtitle_label_, theme->text_color(), 0);
    lv_obj_align(subtitle_label_, LV_ALIGN_BOTTOM_MID, 0, -theme->spacing(4));
    lv_label_set_text(subtitle_label_, "");

    emoji_image_ = lv_image_create(root_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(emoji_image_, LV_ALIGN_CENTER, 0, -12);
    emoji_label_ = lv_label_create(root_);
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_text_color(emoji_label_, theme->text_color(), 0);
    if (large_font != nullptr) {
        lv_obj_set_style_text_font(emoji_label_, large_font, 0);
    }
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);
    lv_obj_align(emoji_label_, LV_ALIGN_CENTER, 0, -12);
#else
    emoji_box_ = lv_obj_create(root_);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    SetTransparent(emoji_box_);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -12);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_color(emoji_label_, theme->text_color(), 0);
    if (large_font != nullptr) {
        lv_obj_set_style_text_font(emoji_label_, large_font, 0);
    }
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);
    emoji_image_ = lv_image_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    auto* top_bar = lv_obj_create(root_);
    lv_obj_set_size(top_bar, LV_HOR_RES, LV_SIZE_CONTENT);
    SetTransparent(top_bar);
    lv_obj_set_style_pad_hor(top_bar, theme->spacing(4), 0);
    lv_obj_set_style_pad_ver(top_bar, theme->spacing(2), 0);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);

    auto* left_icons = lv_obj_create(top_bar);
    lv_obj_set_size(left_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    SetTransparent(left_icons);
    lv_obj_set_style_pad_column(left_icons, theme->spacing(2), 0);
    lv_obj_set_flex_flow(left_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_icons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    network_label_ = lv_label_create(left_icons);
    lv_label_set_text(network_label_, "");
    if (icon_font != nullptr) {
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }
    lv_obj_set_style_text_color(network_label_, theme->text_color(), 0);
    duplex_image_ = lv_image_create(left_icons);
    lv_obj_set_size(duplex_image_, 18, 18);
    lv_obj_add_flag(duplex_image_, LV_OBJ_FLAG_HIDDEN);

    clock_label_ = lv_label_create(top_bar);
    lv_label_set_text(clock_label_, "--:--");
    if (text_font != nullptr) {
        lv_obj_set_style_text_font(clock_label_, text_font, 0);
    }
    lv_obj_set_style_text_color(clock_label_, theme->text_color(), 0);
    lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);

#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    auto* right_icons = lv_obj_create(top_bar);
    // Both status images start hidden. A content-sized parent can therefore be
    // laid out as 0x0 and keep clipping the alarm when it becomes visible later
    // (especially on USB-powered devices where the battery icon stays hidden).
    // Reserve the complete alarm + gap + battery area from the beginning.
    lv_obj_set_size(right_icons, 20 + theme->spacing(2) + 24, 20);
    SetTransparent(right_icons);
    lv_obj_set_style_pad_column(right_icons, theme->spacing(2), 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    alarm_image_ = lv_image_create(right_icons);
    lv_obj_set_size(alarm_image_, 20, 20);
    ResourceData alarm_asset;
    if (Resource::Get(ResourcePackage::Respak,
                           "/icon/alarm/ic_status_alarm.png", alarm_asset)) {
        alarm_icon_ = std::make_unique<LvglRawImage>(
            const_cast<uint8_t*>(alarm_asset.address), alarm_asset.size);
        lv_image_set_src(alarm_image_, alarm_icon_->image_dsc());
    }
    lv_obj_add_flag(alarm_image_, LV_OBJ_FLAG_HIDDEN);

    battery_image_ = lv_image_create(right_icons);
    lv_obj_set_size(battery_image_, 24, 20);
    lv_obj_add_flag(battery_image_, LV_OBJ_FLAG_HIDDEN);
#endif

    status_label_ = lv_label_create(root_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 75 / 100);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, theme->text_color(), 0);
    lv_label_set_text(status_label_, "");
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, theme->spacing(6));

    notification_label_ = lv_label_create(root_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 75 / 100);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_TOP_MID, 0, theme->spacing(6));
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    chat_message_label_ = lv_label_create(root_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, -theme->spacing(4));

    subtitle_label_ = lv_label_create(root_);
    lv_obj_set_width(subtitle_label_, LV_HOR_RES * 90 / 100);
    lv_obj_set_height(subtitle_label_, text_font != nullptr ? text_font->line_height * 3 : 60);
    // The presenter advances sentence pieces itself. Wrap the current piece
    // inside the fixed three-line box instead of replacing its tail with dots.
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(subtitle_label_, theme->text_color(), 0);
    if (text_font != nullptr) {
        lv_obj_set_style_text_font(subtitle_label_, text_font, 0);
    }
    lv_obj_align(subtitle_label_, LV_ALIGN_BOTTOM_MID, 0, -theme->spacing(4));
    lv_label_set_text(subtitle_label_, "");
    lv_obj_add_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
#endif

    music_bar_ = lv_obj_create(root_);
    lv_obj_set_size(music_bar_, LV_HOR_RES * 90 / 100, LV_SIZE_CONTENT);
    SetTransparent(music_bar_);
    lv_obj_set_style_pad_column(music_bar_, theme->spacing(2), 0);
    lv_obj_set_flex_flow(music_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(music_bar_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(music_bar_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(music_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(music_bar_, LV_ALIGN_BOTTOM_MID, 0, -theme->spacing(4));

    music_icon_label_ = lv_label_create(music_bar_);
    lv_label_set_text(music_icon_label_, FONT_AWESOME_MUSIC);
    if (icon_font != nullptr) {
        lv_obj_set_style_text_font(music_icon_label_, icon_font, 0);
    }
    lv_obj_set_style_text_color(music_icon_label_, theme->text_color(), 0);

    music_title_label_ = lv_label_create(music_bar_);
    lv_obj_set_width(music_title_label_, LV_HOR_RES * 72 / 100);
    // 只静态显示当前曲目；超长歌名末尾省略，避免产生列表轮询的观感。
    lv_label_set_long_mode(music_title_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(music_title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(music_title_label_, theme->text_color(), 0);
    if (text_font != nullptr) {
        lv_obj_set_style_text_font(music_title_label_, text_font, 0);
    }
    lv_label_set_text(music_title_label_, "");

#ifdef CONFIG_LSPLATFORM
    image_container_ = lv_obj_create(root_);
    lv_obj_set_size(image_container_, LV_HOR_RES, LV_PCT(85));
    lv_obj_align(image_container_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(image_container_, theme->background_color(), 0);
    lv_obj_set_style_border_width(image_container_, 0, 0);
    lv_obj_add_flag(image_container_, LV_OBJ_FLAG_HIDDEN);
    image_ = lv_image_create(image_container_);
    lv_obj_set_size(image_, LV_PCT(100), LV_PCT(100));
    lv_image_set_align(image_, LV_IMAGE_ALIGN_CONTAIN);
#endif

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void HomeView::DestroyUi() {
    if (notification_timer_ != nullptr) {
        lv_timer_delete(notification_timer_);
        notification_timer_ = nullptr;
    }
    if (animation_player_ != nullptr) {
        animation_player_->Stop();
        animation_player_.reset();
    }
    image_cached_.reset();
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    content_ = nullptr;
    subtitle_label_ = nullptr;
    emoji_box_ = nullptr;
    emoji_label_ = nullptr;
    emoji_image_ = nullptr;
    network_label_ = nullptr;
    duplex_image_ = nullptr;
    clock_label_ = nullptr;
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    alarm_image_ = nullptr;
    alarm_icon_.reset();
    battery_image_ = nullptr;
    battery_icon_.reset();
    battery_icon_index_ = -1;
#endif
    status_label_ = nullptr;
    notification_label_ = nullptr;
    chat_message_label_ = nullptr;
    music_bar_ = nullptr;
    music_icon_label_ = nullptr;
    music_title_label_ = nullptr;
#ifdef CONFIG_LSPLATFORM
    image_container_ = nullptr;
    image_ = nullptr;
#endif
}

void HomeView::Show() {
    if (lvgl_ != nullptr && root_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (display_ != nullptr) {
        display_->SetHomeVisible(true);
    }
}

void HomeView::Hide() {
    if (lvgl_ != nullptr && root_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (display_ != nullptr) {
        display_->SetHomeVisible(false);
#ifdef CONFIG_LSPLATFORM
        display_->DismissImage();
#endif
    }
}

std::string HomeView::ResolveEmotion(const std::string& emotion) const {
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    const auto it = kEmotionAliases.find(emotion);
    return it == kEmotionAliases.end() ? emotion : it->second;
#else
    return emotion;
#endif
}

void HomeView::RenderEmotion(const std::string& emotion, bool one_shot, uint32_t duration_ms) {
    if (lvgl_ == nullptr || emoji_image_ == nullptr || emotion.empty()) {
        return;
    }
    auto* theme = dynamic_cast<LvglTheme*>(display_->GetTheme());
    auto emoji_store = theme != nullptr ? theme->emoji_repository() : nullptr;
    const auto resolved = ResolveEmotion(emotion);
    const EmojiAnimation* animation = emoji_store != nullptr ? emoji_store->GetAnimation(resolved) : nullptr;
    const auto* image = emoji_store != nullptr ? emoji_store->GetEmojiImage(resolved.c_str()) : nullptr;

    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }
    if (animation_player_ != nullptr) {
        animation_player_->Stop();
    }
    if (animation != nullptr && (!animation->intro.empty() || !animation->loop.empty() || !animation->outro.empty())) {
        if (animation_player_ == nullptr) {
            animation_player_ = std::make_unique<PngAnimationPlayer>(emoji_image_);
        }
        if (one_shot) {
            one_shot_emotion_active_ = true;
            animation_player_->PlayOnce(animation, duration_ms, [this]() {
                UiManager::GetInstance().Dispatch([this]() {
                    one_shot_emotion_active_ = false;
                    if (!current_emotion_.empty()) {
                        RenderEmotion(current_emotion_, false);
                    }
                });
            });
        } else {
            animation_player_->Play(animation);
        }
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (image != nullptr) {
        lv_image_set_src(emoji_image_, image->image_dsc());
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    } else if (emoji_label_ != nullptr) {
        const char* utf8 = font_awesome_get_utf8(resolved.c_str());
        if (utf8 != nullptr) {
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    if (resolved == "neutral" && content_ != nullptr && lv_obj_get_child_cnt(content_) > 0) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}

void HomeView::SetEmotion(const std::string& emotion) {
    if (emotion.empty()) {
        return;
    }
    current_emotion_ = emotion;
    if (!one_shot_emotion_active_) {
        if (lvgl_ != nullptr) {
            RenderEmotion(emotion, false);
        } else if (display_ != nullptr) {
            display_->SetEmotion(emotion.c_str());
        }
    }
}

void HomeView::ShowEmotionOnce(const std::string& emotion) {
    if (lvgl_ != nullptr) {
        RenderEmotion(emotion, true, 3000);
    } else if (display_ != nullptr) {
        display_->ShowEmotionOnce(emotion.c_str());
    }
}

void HomeView::SetPowerSaveMode(bool enabled) {
    SetEmotion(enabled ? "sleepy" : "neutral");
}

void HomeView::SetText(const std::string& text) {
    SetChatMessage("system", text);
}

void HomeView::SetSubtitleText(const std::string& text) {
    if (lvgl_ == nullptr) {
        if (display_ != nullptr) {
            display_->SetChatMessage("system", text.c_str());
        }
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }
    if (subtitle_label_ != nullptr) {
        lv_label_set_text(subtitle_label_, text.c_str());
        if (text.empty()) {
            lv_obj_add_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
            // Image/animation widgets may be created or refreshed after the
            // subtitle label. Keep spoken text on the top visual layer.
            lv_obj_move_foreground(subtitle_label_);
        }
    }
#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Timeline data can arrive while audio is still prebuffering. Clearing the
    // old subtitle at that point must not hide the final IAT text during the
    // thinking phase. Hide it only when a real TTS subtitle is displayed.
    if (!text.empty() && chat_message_label_ != nullptr) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void HomeView::SetChatMessage(const std::string& role, const std::string& text) {
    if (lvgl_ == nullptr) {
        if (display_ != nullptr) {
            display_->SetChatMessage(role.c_str(), text.c_str());
        }
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    if (content_ == nullptr || text.empty()) {
        return;
    }
    constexpr uint32_t kMaxMessages = 40;
    if (lv_obj_get_child_cnt(content_) >= kMaxMessages) {
        lv_obj_delete(lv_obj_get_child(content_, 0));
    }
    auto* theme = dynamic_cast<LvglTheme*>(display_->GetTheme());
    auto* bubble = lv_obj_create(content_);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, theme != nullptr ? theme->spacing(4) : 8, 0);
    auto* label = lv_label_create(bubble);
    lv_label_set_text(label, text.c_str());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_HOR_RES * 85 / 100);
    if (theme != nullptr) {
        lv_obj_set_style_bg_color(bubble,
            role == "user" ? theme->user_bubble_color() :
            role == "system" ? theme->system_bubble_color() : theme->assistant_bubble_color(), 0);
        lv_obj_set_style_text_color(label,
            role == "system" ? theme->system_text_color() : theme->text_color(), 0);
    }
    lv_obj_scroll_to_view_recursive(bubble, LV_ANIM_ON);
#else
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, text.c_str());
        lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (subtitle_label_ != nullptr) {
        lv_obj_add_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (role != "system" && emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void HomeView::ShowNotification(const std::string& text, int duration_ms) {
    if (lvgl_ == nullptr || notification_label_ == nullptr) {
        if (display_ != nullptr) {
            display_->ShowNotification(text, duration_ms);
        }
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }
    lv_label_set_text(notification_label_, text.c_str());
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    if (status_label_ != nullptr) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (notification_timer_ != nullptr) {
        lv_timer_delete(notification_timer_);
    }
    notification_timer_ = lv_timer_create(&HomeView::NotificationTimerCallback,
                                          duration_ms > 0 ? duration_ms : 1, this);
    lv_timer_set_repeat_count(notification_timer_, 1);
}

void HomeView::NotificationTimerCallback(lv_timer_t* timer) {
    auto* view = static_cast<HomeView*>(lv_timer_get_user_data(timer));
    if (view != nullptr) {
        if (view->notification_label_ != nullptr) {
            lv_obj_add_flag(view->notification_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (view->status_label_ != nullptr) {
            lv_obj_remove_flag(view->status_label_, LV_OBJ_FLAG_HIDDEN);
        }
        view->notification_timer_ = nullptr;
    }
    lv_timer_delete(timer);
}

void HomeView::SetWifiIcon(const std::string& icon) {
    if (lvgl_ != nullptr && network_label_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            lv_label_set_text(network_label_, icon.c_str());
        }
    } else if (display_ != nullptr) {
        display_->SetNetworkIcon(icon.c_str());
    }
}

void HomeView::SetAlarmActive(bool active) {
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    if (lvgl_ == nullptr || alarm_image_ == nullptr) return;

    DisplayLockGuard lock(display_);
    if (!lock.locked()) return;
    if (!active) {
        lv_obj_add_flag(alarm_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(lv_obj_get_parent(alarm_image_));
        return;
    }

    if (alarm_icon_ == nullptr) {
        ResourceData asset;
        if (!Resource::Get(ResourcePackage::Respak,
                                "/icon/alarm/ic_status_alarm.png", asset)) {
            lv_obj_add_flag(alarm_image_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        alarm_icon_ = std::make_unique<LvglRawImage>(
            const_cast<uint8_t*>(asset.address), asset.size);
        lv_image_set_src(alarm_image_, alarm_icon_->image_dsc());
    }
    lv_obj_remove_flag(alarm_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(lv_obj_get_parent(alarm_image_));
#else
    (void)active;
#endif
}

void HomeView::SetBattery(int level, bool charging) {
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    if (lvgl_ == nullptr || battery_image_ == nullptr) {
        return;
    }

    // The BOX3 ADC estimator reports zero capacity when the battery input is
    // below the valid battery-voltage range (including no battery connected).
    // Keep power0 for a real 1%-10% battery, but hide the indicator for this
    // zero-capacity/no-battery state.
    if (level <= 0) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            lv_obj_add_flag(battery_image_, LV_OBJ_FLAG_HIDDEN);
            battery_icon_.reset();
            battery_icon_index_ = -1;
        }
        return;
    }

    const int clamped_level = std::max(0, std::min(level, 100));
    // The asset set has ten levels: power0/charging0 represents 10% or less,
    // and power9/charging9 represents the 91-100% range.
    const int icon_index = clamped_level <= 10 ? 0 : std::min(9, (clamped_level - 1) / 10);

    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }

    if (battery_icon_ == nullptr || battery_icon_index_ != icon_index ||
        battery_icon_charging_ != charging) {
        const std::string path = std::string(charging ? "/icon/battery/ic_status_charging" :
                                              "/icon/battery/ic_status_power") +
                                 std::to_string(icon_index) + ".png";
        ResourceData asset;
        if (!Resource::Get(ResourcePackage::Respak, path, asset)) {
            lv_obj_add_flag(battery_image_, LV_OBJ_FLAG_HIDDEN);
            battery_icon_.reset();
            battery_icon_index_ = -1;
            return;
        }
        battery_icon_ = std::make_unique<LvglRawImage>(
            const_cast<uint8_t*>(asset.address), asset.size);
        battery_icon_index_ = icon_index;
        battery_icon_charging_ = charging;
    }

    lv_image_set_src(battery_image_, battery_icon_->image_dsc());
    lv_obj_remove_flag(battery_image_, LV_OBJ_FLAG_HIDDEN);
#else
    (void)level;
    (void)charging;
#endif
}

void HomeView::SetStatus(const std::string& status) {
    if (lvgl_ != nullptr && status_label_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            lv_label_set_text(status_label_, status.c_str());
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            if (notification_label_ != nullptr) {
                lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else if (display_ != nullptr) {
        display_->SetStatus(status.c_str());
    }
}

void HomeView::SetMusicTrack(const MusicTrack& track, bool visible) {
    if (lvgl_ == nullptr || music_bar_ == nullptr || music_title_label_ == nullptr) {
        if (display_ != nullptr && visible) {
            display_->SetChatMessage("system", track.name.c_str());
        }
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }
    if (!visible || track.name.empty()) {
        lv_obj_add_flag(music_bar_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(music_title_label_, track.name.c_str());
    lv_obj_remove_flag(music_bar_, LV_OBJ_FLAG_HIDDEN);
    if (chat_message_label_ != nullptr) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(music_bar_);
}

void HomeView::SetTime(const std::string& time) {
    if (lvgl_ != nullptr && clock_label_ != nullptr && !time.empty()) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            lv_label_set_text(clock_label_, time.c_str());
            lv_obj_remove_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (display_ != nullptr && !time.empty()) {
        display_->SetClockText(time.c_str());
    }
}

#ifdef CONFIG_LSPLATFORM
void HomeView::ShowImage(std::unique_ptr<LvglImage> image) {
    if (image == nullptr) {
        return;
    }
    if (lvgl_ == nullptr || image_container_ == nullptr || image_ == nullptr) {
        if (display_ != nullptr) {
            display_->ShowImage(std::move(image));
        }
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock.locked()) {
        return;
    }
    image_cached_ = std::move(image);
    lv_image_set_src(image_, image_cached_->image_dsc());
    lv_obj_remove_flag(image_container_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
}

void HomeView::HideImage() {
    if (lvgl_ != nullptr && image_container_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (lock.locked()) {
            lv_obj_add_flag(image_container_, LV_OBJ_FLAG_HIDDEN);
            if (image_ != nullptr) {
                lv_image_set_src(image_, nullptr);
            }
            if (emoji_box_ != nullptr) {
                lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
            image_cached_.reset();
        }
    } else if (display_ != nullptr) {
        display_->DismissImage();
    }
}
#endif

void HomeView::SetDuplexMode(DuplexMode mode) {
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    if (lvgl_ != nullptr && duplex_image_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (!lock.locked()) {
            return;
        }
        if (mode == DuplexMode::kSimplex) {
            lv_obj_add_flag(duplex_image_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        const bool full_duplex = mode == DuplexMode::kFullDuplex;
        const char* path = full_duplex
            ? "/icon/duplux/ic_status_duplex_interruptible.png"
            : "/icon/duplux/ic_status_duplex_non_interruptible.png";
        auto& icon = full_duplex ? full_duplex_icon_ : half_duplex_icon_;
        if (!icon) {
            ResourceData asset;
            if (!Resource::Get(ResourcePackage::Respak, path, asset)) {
                lv_obj_add_flag(duplex_image_, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            icon = std::make_unique<LvglRawImage>(
                const_cast<uint8_t*>(asset.address), asset.size);
        }
        lv_image_set_src(duplex_image_, icon->image_dsc());
        lv_obj_remove_flag(duplex_image_, LV_OBJ_FLAG_HIDDEN);
    } else if (display_ != nullptr) {
        display_->SetDuplexMode(mode);
    }
#else
    if (display_ != nullptr) {
        display_->SetDuplexMode(mode);
    }
#endif
}
