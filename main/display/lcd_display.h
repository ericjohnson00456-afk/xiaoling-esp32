#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "display/lvgl_display.h"
#include "ui/utils/emoji/png_animation_player.h"
#include "display_refresh_stats.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <memory>

class SpiBusArbiter;

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    lv_obj_t* duplex_image_ = nullptr;
#endif
    std::unique_ptr<PngAnimationPlayer> animation_player_ = nullptr;
    std::string current_emotion_;
    bool one_shot_emotion_active_ = false;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
#ifndef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    esp_timer_handle_t preview_timer_ = nullptr;
#endif // !CONFIG_USE_XIAOLING_MESSAGE_STYLE
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

#ifdef CONFIG_LSPLATFORM
    lv_obj_t* activation_container_ = nullptr;
    lv_obj_t* activation_qrcode_ = nullptr;
    lv_obj_t* activation_message_ = nullptr;
    std::unique_ptr<LvglImage> activation_qrcode_cached_ = nullptr;

    lv_obj_t* image_container_ = nullptr;
    lv_obj_t* image_ = nullptr;
    std::unique_ptr<LvglImage> image_cached_ = nullptr;
#endif // CONFIG_LSPLATFORM

    void SetupUI();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void ShowEmotionOnce(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetHomeVisible(bool visible) override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);

#ifdef CONFIG_LSPLATFORM
    void SetupActivationUI();
    virtual void ShowActivation(std::unique_ptr<LvglImage> qrcode, const std::string& message) override;
    virtual void DismissActivation() override;

    void SetupImageUI();
    virtual void ShowImage(std::unique_ptr<LvglImage> image) override;
    virtual void DismissImage() override;
#endif // CONFIG_LSPLATFORM
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
private:
    SpiBusArbiter* spi_bus_arbiter_ = nullptr;
    // 一次 LVGL 刷新可能被绘制缓冲拆成多个 flush；整个刷新期间只持有一次总线。
    std::atomic_bool refresh_bus_locked_{false};
    std::atomic_bool release_bus_on_flush_done_{false};
    DisplayRefreshStats refresh_stats_;
    SemaphoreHandle_t flush_ready_ = nullptr;

    static void RefreshStatsEvent(lv_event_t* event);
    static void BlockingFlushWait(lv_display_t* display);

    static void SharedBusFlushStart(lv_event_t* event);
    static bool SharedBusFlushDone(esp_lcd_panel_io_handle_t panel_io,
                                   esp_lcd_panel_io_event_data_t* event_data,
                                   void* user_context);

public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,
                  SpiBusArbiter* spi_bus_arbiter = nullptr);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
