#include "home_presenter.h"

#include "display/display.h"
#include "board.h"
#include "home_view.h"
#include "ui/i18n/lang_config.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"
#include "ui/utils/image/lvgl_image.h"
#include "system/device_status.h"

#include <utility>

#include <esp_log.h>

#define TAG "HomePresenter"

#ifdef CONFIG_LSPLATFORM
namespace {

class SharedLvglImage final : public LvglImage {
public:
    explicit SharedLvglImage(std::shared_ptr<LvglImage> image) : image_(std::move(image)) {
    }

    const lv_img_dsc_t* image_dsc() const override {
        return image_ != nullptr ? image_->image_dsc() : nullptr;
    }

private:
    std::shared_ptr<LvglImage> image_;
};

}  // namespace
#endif

HomePresenter& HomePresenter::GetInstance() {
    static HomePresenter instance;
    return instance;
}

HomePresenter::HomePresenter()
    : voice_subtitles_(*this) {
}

const UiNavScreen& HomePresenter::Screen() {
    static const UiNavScreen screen = {
        .unique_id = UI_PAGE_HOME,
        .open = &HomePresenter::Open,
        .show = &HomePresenter::Show,
        .pause = &HomePresenter::Pause,
        .resume = &HomePresenter::Resume,
        .close = &HomePresenter::Close,
    };
    return screen;
}

void HomePresenter::SetWakeWord(std::string wake_word) {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    wake_word_ = std::move(wake_word);
}

int HomePresenter::Open(const UiNavScreen* screen, void** data) {
    (void)screen;
    auto& presenter = GetInstance();
    presenter.view_ = std::make_unique<HomeView>(UiManager::GetInstance().display());
    presenter.voice_subtitles_.Start();
    *data = &presenter;
    return 0;
}

int HomePresenter::Show(const UiNavScreen* screen, void* data) {
    (void)screen;
    auto* presenter = static_cast<HomePresenter*>(data);
    if (presenter == nullptr || presenter->view_ == nullptr) {
        return -1;
    }
    presenter->view_->Show();
    presenter->SetEmotion(presenter->last_emotion_);
    presenter->SetChatMessage(presenter->last_chat_role_, presenter->last_chat_text_);
    presenter->SetText(presenter->last_text_);
    presenter->SetWifiIcon(presenter->wifi_icon_);
    presenter->SetTfCardPresent(presenter->tf_card_present_);
    presenter->SetAlarmActive(presenter->alarm_active_);
    presenter->UpdateBattery();
    presenter->SetStatus(presenter->status_text_, presenter->standby_);
    presenter->SetTime(presenter->time_text_);
    presenter->SetDuplexVisible(presenter->duplex_visible_);
#ifdef CONFIG_LSPLATFORM
    if (presenter->image_ != nullptr) {
        presenter->view_->ShowImage(std::make_unique<SharedLvglImage>(presenter->image_));
    }
#endif
    return 0;
}

int HomePresenter::Pause(const UiNavScreen* screen, void* data) {
    (void)screen;
    auto* presenter = static_cast<HomePresenter*>(data);
    if (presenter != nullptr && presenter->view_ != nullptr) {
        presenter->view_->Hide();
    }
    return 0;
}

int HomePresenter::Resume(const UiNavScreen* screen, void* data) {
    return Show(screen, data);
}

int HomePresenter::Close(const UiNavScreen* screen, void* data) {
    (void)screen;
    auto* presenter = static_cast<HomePresenter*>(data);
    if (presenter == nullptr) {
        return 0;
    }
    presenter->voice_subtitles_.Stop();
    presenter->StopBannerTimer();
    presenter->view_.reset();
    presenter->last_emotion_.clear();
    presenter->last_chat_role_.clear();
    presenter->last_chat_text_.clear();
    return 0;
}

void HomePresenter::SetEmotion(std::string emotion) {
    last_emotion_ = std::move(emotion);
    if (view_ != nullptr && !last_emotion_.empty()) {
        view_->SetEmotion(last_emotion_);
    }
}

void HomePresenter::ShowEmotionOnce(std::string emotion) {
    if (view_ != nullptr) {
        view_->ShowEmotionOnce(emotion);
    }
}

void HomePresenter::SetPowerSaveMode(bool enabled) {
    SetEmotion(enabled ? "sleepy" : "neutral");
    SetChatMessage("system", "");
}

void HomePresenter::SetText(std::string text) {
    last_text_ = std::move(text);
    if (view_ != nullptr && !last_text_.empty()) {
        view_->SetText(last_text_);
    }
}

void HomePresenter::SetSubtitleText(std::string text, uint8_t max_lines) {
    ESP_LOGI(TAG, "更新 Home 字幕: bytes=%u, max_lines=%u, top_page=%d, view=%d",
             static_cast<unsigned>(text.size()),
             static_cast<unsigned>(max_lines),
             UiManager::GetInstance().TopScreenId(), view_ != nullptr ? 1 : 0);
    if (view_ != nullptr) {
        view_->SetSubtitleText(text, max_lines);
    }
}

void HomePresenter::SetChatMessage(std::string role, std::string text) {
    last_chat_role_ = role;
    last_chat_text_ = text;
    if (view_ != nullptr) {
        view_->SetChatMessage(std::move(role), std::move(text));
    }
}

void HomePresenter::ShowNotification(std::string text, int duration_ms) {
    if (view_ != nullptr && !text.empty()) {
        view_->ShowNotification(text, duration_ms);
    }
}

void HomePresenter::SetWifiIcon(std::string icon) {
    wifi_icon_ = std::move(icon);
    if (view_ != nullptr) {
        view_->SetWifiIcon(wifi_icon_);
    }
}

void HomePresenter::SetTfCardPresent(bool present) {
    tf_card_present_ = present;
    if (view_ != nullptr) view_->SetTfCardPresent(present);
}

void HomePresenter::SetAlarmActive(bool active) {
    alarm_active_ = active;
    if (view_ != nullptr) {
        view_->SetAlarmActive(active);
    }
}

void HomePresenter::SetChargingAnimationReady(bool ready) {
    charging_animation_ready_ = ready;
    if (!ready) {
        return;
    }

    // 连接成功时建立新的充电基线。若 USB 在连接建立前已经插入，
    // 这里同步状态但不播放动画；之后的 false -> true 才代表新插入。
    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        return;
    }
    battery_level_ = level;
    battery_charging_ = charging;
    if (view_ != nullptr) {
        view_->SetBattery(battery_level_, battery_charging_);
    }
}

void HomePresenter::UpdateBattery() {
    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        return;
    }

    // 充电表情是插入 USB 时的提示动画。只在充电状态发生上升沿时播放，
    // 避免状态栏每秒刷新时反复重启动画。
    const bool charging_started = charging && !battery_charging_;
    battery_level_ = level;
    battery_charging_ = charging;
    if (view_ != nullptr) {
        view_->SetBattery(battery_level_, battery_charging_);
        if (charging_started && charging_animation_ready_) {
            view_->ShowEmotionOnce("充电");
        }
    }
}

void HomePresenter::SetStatus(std::string status, bool standby) {
    if (standby && !service_connected_) {
        status = Lang::GetLanguage() == Lang::Language::English
                     ? "Service disconnected" : "服务未连接";
    }
    status_text_ = std::move(status);
    standby_ = standby;
    if (view_ != nullptr) {
        view_->SetStatus(status_text_);
        RenderMusicTrack();
    }
}

void HomePresenter::SetServiceConnected(bool connected) {
    service_connected_ = connected;
    if (!connected || standby_) {
        SetStatus(Lang::Strings::STANDBY, true);
    }
}

void HomePresenter::SetMusicTrack(MusicTrack track) {
    music_track_ = std::move(track);
    RenderMusicTrack();
}

void HomePresenter::SetMusicPaused(bool paused) {
    music_paused_ = paused;
    RenderMusicTrack();
}

void HomePresenter::RenderMusicTrack() {
    if (view_ != nullptr) {
        view_->SetMusicTrack(
            music_track_,
            standby_ && !voice_subtitles_.IsTtsPlaying() &&
                !music_paused_ && !music_track_.name.empty());
    }
}

void HomePresenter::SetTime(std::string time) {
    time_text_ = std::move(time);
    if (view_ != nullptr && !time_text_.empty()) {
        view_->SetTime(time_text_);
    }
}

void HomePresenter::SetDuplexMode(DuplexMode mode) {
    duplex_mode_ = mode;
    if (duplex_visible_ && view_ != nullptr) {
        view_->SetDuplexMode(mode);
    }
}

void HomePresenter::SetDuplexVisible(bool visible) {
    duplex_visible_ = visible;
    if (view_ != nullptr) {
        view_->SetDuplexMode(visible ? duplex_mode_ : DuplexMode::kSimplex);
    }
}

#ifdef CONFIG_LSPLATFORM
void HomePresenter::SetImage(std::unique_ptr<LvglImage> image) {
    if (image == nullptr) {
        return;
    }
    image_ = std::shared_ptr<LvglImage>(std::move(image));
    if (view_ != nullptr && UiManager::GetInstance().TopScreenId() == UI_PAGE_HOME) {
        view_->ShowImage(std::make_unique<SharedLvglImage>(image_));
    }
}

void HomePresenter::ClearImage() {
    image_.reset();
    if (view_ != nullptr) {
        view_->HideImage();
    }
}
#endif

void HomePresenter::HandleSettingMessage(const cJSON* data) {
    const auto* theme = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "theme") : nullptr;
    const auto* frontend = cJSON_IsObject(theme) ? cJSON_GetObjectItemCaseSensitive(theme, "frontend") : nullptr;
    const auto* banner = cJSON_IsObject(frontend) ? cJSON_GetObjectItemCaseSensitive(frontend, "banner") : nullptr;
    if (!cJSON_IsObject(banner)) {
        return;
    }

    uint32_t interval_ms = 3000;
    const auto* interval = cJSON_GetObjectItemCaseSensitive(banner, "interval_ms");
    if (cJSON_IsNumber(interval) && interval->valueint >= 1000) {
        interval_ms = static_cast<uint32_t>(interval->valueint);
    }

    std::vector<std::string> texts;
    const auto* resources = cJSON_GetObjectItemCaseSensitive(banner, "resources");
    if (cJSON_IsArray(resources)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, resources) {
            const auto* text = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "text") : nullptr;
            if (cJSON_IsString(text) && text->valuestring != nullptr && text->valuestring[0] != '\0') {
                texts.push_back(SubstituteWakeWord(text->valuestring));
            }
        }
    }
    SetBanners(std::move(texts), interval_ms);
}

std::string HomePresenter::SubstituteWakeWord(const std::string& text) const {
    static constexpr const char* kPlaceholder = "#唤醒词#";
    if (text.find(kPlaceholder) == std::string::npos) {
        return text;
    }
    std::string wake_word;
    {
        std::lock_guard<std::mutex> lock(wake_word_mutex_);
        wake_word = wake_word_;
    }
    if (wake_word.empty()) {
        return text;
    }
    std::string result = text;
    size_t pos = 0;
    while ((pos = result.find(kPlaceholder, pos)) != std::string::npos) {
        result.replace(pos, std::strlen(kPlaceholder), wake_word);
        pos += wake_word.size();
    }
    return result;
}

void HomePresenter::SetBanners(std::vector<std::string> texts, uint32_t interval_ms) {
    StopBannerTimer();
    bool has_banners = false;
    std::string first_text;
    {
        std::lock_guard<std::mutex> lock(banner_mutex_);
        banner_texts_ = std::move(texts);
        banner_index_ = 0;
        has_banners = !banner_texts_.empty();
        if (has_banners && (music_paused_ || music_track_.name.empty()) &&
            DeviceStatus::GetState() == kDeviceStateIdle) {
            first_text = banner_texts_[banner_index_];
            banner_index_ = (banner_index_ + 1) % banner_texts_.size();
        }
    }
    if (!has_banners) {
        return;
    }

    // Show the first banner immediately. Waiting for the first periodic tick
    // makes startup appear blocked by the role-config image download.
    if (!first_text.empty() && view_ != nullptr) {
        view_->SetText(first_text);
        ESP_LOGI(TAG, "首条 Banner 已显示");
    }

    esp_timer_create_args_t args = {
        .callback = &HomePresenter::BannerTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "home_banner",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &banner_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "创建 Home Banner 定时器失败");
        banner_timer_ = nullptr;
        return;
    }
    esp_timer_start_periodic(banner_timer_, static_cast<uint64_t>(interval_ms) * 1000);
}

void HomePresenter::StopBannerTimer() {
    if (banner_timer_ != nullptr) {
        esp_timer_stop(banner_timer_);
        esp_timer_delete(banner_timer_);
        banner_timer_ = nullptr;
    }
}

void HomePresenter::BannerTimerCallback(void* arg) {
    static_cast<HomePresenter*>(arg)->OnBannerTick();
}

void HomePresenter::OnBannerTick() {
    UiManager::GetInstance().Dispatch([]() {
        auto& home = HomePresenter::GetInstance();
        // 歌曲名称占用待命内容区域时暂停 Banner。这里在 UI 队列中再次
        // 判断，避免定时器投递后、命令执行前恰好发生曲目切换。
        if (home.view_ == nullptr || !home.standby_ ||
            (!home.music_paused_ && !home.music_track_.name.empty()) ||
            DeviceStatus::GetState() != kDeviceStateIdle) {
            return;
        }
        std::string text;
        {
            std::lock_guard<std::mutex> lock(home.banner_mutex_);
            if (home.banner_texts_.empty()) {
                return;
            }
            text = home.banner_texts_[home.banner_index_];
            home.banner_index_ =
                (home.banner_index_ + 1) % home.banner_texts_.size();
        }
        home.view_->SetText(text);
    });
}
