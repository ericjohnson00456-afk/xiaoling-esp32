#pragma once

#include "ui/utils/image/lvgl_image.h"
#include "ui/utils/emoji/png_animation_player.h"
#include "player/types/music_track.h"
#include "subtitle_pagination.h"

#include <lvgl.h>

#include <memory>
#include <string>

class Display;
class LvglDisplay;
enum class DuplexMode;

// Home page view. It owns the page widgets and is the only layer that knows
// their LVGL layout. Presenter methods only provide values to render.
class HomeView {
public:
    explicit HomeView(Display* display);
    ~HomeView();

    void Show();
    void Hide();
    void SetEmotion(const std::string& emotion);
    void ShowEmotionOnce(const std::string& emotion);
    void SetPowerSaveMode(bool enabled);
#ifdef CONFIG_LSPLATFORM
    void ShowImage(std::unique_ptr<LvglImage> image);
    void HideImage();
#endif
    void SetText(const std::string& text);
    std::vector<SubtitlePagination::Page> PaginateSubtitle(const std::string& text,
                                                         uint8_t max_lines = 3);
    void SetSubtitleText(const std::string& text, uint8_t max_lines = 3);
    void SetChatMessage(const std::string& role, const std::string& text);
    void ShowNotification(const std::string& text, int duration_ms);
    void SetWifiIcon(const std::string& icon);
    void SetTfCardPresent(bool present);
    void SetAlarmActive(bool active);
    void SetBattery(int level, bool charging);
    void SetStatus(const std::string& status);
    void SetMusicTrack(const MusicTrack& track, bool visible);
    void SetTime(const std::string& time);
    void SetDuplexMode(DuplexMode mode);

private:
    void CreateUi();
    void DestroyUi();
    void RenderEmotion(const std::string& emotion, bool one_shot,
                       uint32_t duration_ms = 0);
    std::string ResolveEmotion(const std::string& emotion) const;
    static void NotificationTimerCallback(lv_timer_t* timer);

    Display* display_ = nullptr;
    LvglDisplay* lvgl_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* subtitle_label_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* tf_card_label_ = nullptr;
    lv_obj_t* duplex_image_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* music_bar_ = nullptr;
    lv_obj_t* music_icon_label_ = nullptr;
    lv_obj_t* music_title_label_ = nullptr;
#ifdef CONFIG_LSPLATFORM
    lv_obj_t* image_container_ = nullptr;
    lv_obj_t* image_ = nullptr;
#endif
    lv_timer_t* notification_timer_ = nullptr;
    std::unique_ptr<PngAnimationPlayer> animation_player_;
    std::unique_ptr<LvglImage> image_cached_;
    std::string current_emotion_;
    bool one_shot_emotion_active_ = false;
    bool image_visible_ = false;
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    std::unique_ptr<LvglRawImage> half_duplex_icon_;
    std::unique_ptr<LvglRawImage> full_duplex_icon_;
    lv_obj_t* alarm_image_ = nullptr;
    std::unique_ptr<LvglRawImage> alarm_icon_;
    lv_obj_t* battery_image_ = nullptr;
    std::unique_ptr<LvglRawImage> battery_icon_;
    int battery_icon_index_ = -1;
    bool battery_icon_charging_ = false;
#endif
};
