#include "ui.h"

#include "alarm/alarm_service.h"
#include "board.h"
#include "display/display.h"
#include "system/device_status.h"
#include "system/settings.h"
#include "shell/cmd/kv/kv_user.h"
#include "ui/i18n/lang_config.h"
#include "ui/screens/home/home_presenter.h"
#include "ui/screens/alarm/alarm_ring_presenter.h"
#include "ui/screens/alarm/alarm_success_presenter.h"
#include "ui/screens/camera_preview/camera_preview_presenter.h"
#include "ui/screens/qrcode/qrcode_presenter.h"
#include "ui/screens/upgrade/upgrade_presenter.h"
#include "ui/screens/sd_music_sync/sd_music_sync_presenter.h"
#include "ui/themes/lvgl_theme.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"
#include "ui/utils/emoji/emoji_repository.h"
#include "ui/utils/image/lvgl_image.h"
#include "ui/widgets/toast.h"

#include <esp_log.h>
#include <cJSON.h>

#include <string>
#include <algorithm>
#include <cstdio>
#include <optional>
#include <utility>

namespace {

constexpr char kTag[] = "Ui";
constexpr char kUiSettingsNamespace[] = "ui";
constexpr char kLanguageSettingsKey[] = "language";
std::optional<Ui::Status> current_localized_status;

std::string LocalizeStatus(Ui::Status status) {
    switch (status) {
        case Ui::Status::Initializing:
            return Lang::Strings::INITIALIZING;
        case Ui::Status::ScanningWifi:
            return Lang::Strings::SCANNING_WIFI;
        case Ui::Status::RegisteringNetwork:
            return Lang::Strings::REGISTERING_NETWORK;
        case Ui::Status::DetectingModule:
            return Lang::Strings::DETECTING_MODULE;
        case Ui::Status::CheckingNewVersion:
            return Lang::Strings::CHECKING_NEW_VERSION;
        case Ui::Status::LoadingProtocol:
            return Lang::Strings::LOADING_PROTOCOL;
        case Ui::Status::Standby:
            return Lang::Strings::STANDBY;
        case Ui::Status::Connecting:
            return Lang::Strings::CONNECTING;
        case Ui::Status::Listening:
            return Lang::Strings::LISTENING;
        case Ui::Status::Speaking:
            return Lang::Strings::SPEAKING;
    }
    return Lang::Strings::INITIALIZING;
}

std::string LocalizeNotification(Ui::Notification notification, const std::string& detail) {
    switch (notification) {
        case Ui::Notification::ScanningWifi:
            return Lang::Strings::SCANNING_WIFI;
        case Ui::Notification::ConnectingToNetwork:
            return std::string(Lang::Strings::CONNECT_TO) + detail + "...";
        case Ui::Notification::ConnectedToNetwork:
            return std::string(Lang::Strings::CONNECTED_TO) + detail;
        case Ui::Notification::SwitchTo4G:
            return Lang::Strings::SWITCH_TO_4G_NETWORK;
        case Ui::Notification::SwitchToWifi:
            return Lang::Strings::SWITCH_TO_WIFI_NETWORK;
        case Ui::Notification::EnteringWifiConfigMode:
            return Lang::Strings::ENTERING_WIFI_CONFIG_MODE;
    }
    return {};
}

void ShowNotificationOnDisplay(std::string text, int32_t duration_ms) {
    if (UiManager::GetInstance().lvgl_display() != nullptr) {
        HomePresenter::GetInstance().ShowNotification(std::move(text), duration_ms);
    } else if (auto* display = UiManager::GetInstance().display(); display != nullptr) {
        display->ShowNotification(text, duration_ms);
    }
}

void UpdateStatusBarOnUi(bool update_all) {
    auto& manager = UiManager::GetInstance();
    if (manager.lvgl_display() != nullptr) {
        auto& home = HomePresenter::GetInstance();
        home.SetAlarmActive(AlarmService::GetInstance().HasActiveCountdown());
        home.UpdateBattery();
    } else if (auto* display = manager.display(); display != nullptr) {
        display->UpdateStatusBar(update_all);
    }
}

void ShowVolumeText(std::string text) {
    UiManager::GetInstance().Dispatch([text = std::move(text)]() mutable {
        ShowNotificationOnDisplay(std::move(text), 3000);
    });
}

bool ShouldIgnoreStandbyEmotion(const std::string& emotion) {
    const auto state = DeviceStatus::GetState();
    const bool conversation_active = state == kDeviceStateConnecting ||
                                     state == kDeviceStateListening ||
                                     state == kDeviceStateSpeaking;
    const bool standby_emotion = emotion == "待机" || emotion == "neutral" || emotion == "relaxed";
    if (!conversation_active || !standby_emotion) {
        return false;
    }
    ESP_LOGI(kTag, "Ignoring standby emotion while device is in %s state",
             DeviceStatus::GetStateName(state));
    return true;
}

}  // namespace

namespace Ui {

bool Initialize() {
    // 第一步：确定界面语言。用户 KV 的显式选择优先，其次使用 UI 自己保存的语言。
    Settings settings(kUiSettingsNamespace);
    const auto saved_language = settings.GetString(kLanguageSettingsKey);
    const auto user_language = KvUser::GetUiLanguage();
    if (user_language == 0 || user_language == 1) {
        Lang::SetLanguage(user_language == 1 ? Lang::Language::English
                                             : Lang::Language::Primary);
    } else if (!saved_language.empty() && !Lang::SetLanguageByCode(saved_language.c_str())) {
        ESP_LOGW(kTag, "Ignoring unsupported saved language: %s", saved_language.c_str());
    }

    // 第二步：从 Board 取得显示设备并绑定到 UiManager。
    // Display 的面板和 LVGL 底层已经由 Board 创建，UI 这里只保存指针，不重复初始化硬件。
    auto& ui = UiManager::GetInstance();
    if (!ui.initialized()) {
        auto& board = Board::GetInstance();
        auto* display = board.GetDisplay();
        if (display == nullptr) {
            ESP_LOGE(kTag, "Board did not provide a display");
            return false;
        }
        ui.Initialize(display);
    }

    // 第三步：把所有页面加入导航表，并把 Home 设置为默认页面。
    // 注册阶段只建立页面描述和导航关系，还没有启动 UI 命令任务。
    if (ui.RegisterScreen(&HomePresenter::Screen()) != 0 ||
        ui.RegisterScreen(&AlarmSuccessPresenter::Screen()) != 0 ||
        ui.RegisterScreen(&AlarmRingPresenter::Screen()) != 0 ||
        ui.RegisterScreen(&CameraPreviewPresenter::Screen()) != 0 ||
        ui.RegisterScreen(&QrCodePresenter::Screen()) != 0 ||
        ui.RegisterScreen(&UpgradePresenter::Screen()) != 0 ||
        ui.RegisterScreen(&SdMusicSyncPresenter::Screen()) != 0 ||
        ui.SetDefaultScreen(UI_PAGE_HOME) != 0) {
        return false;
    }

    // 第四步：最后启动 UI 专用任务。此后 Ui::SetXxx()/ShowXxx() 只负责投递命令，
    // 真正的 Presenter/View 更新都由该任务按 FIFO 顺序执行。
    return ui.StartTask();
}

void SetStatus(Status status) {
    // 这里只把操作放入队列，不在调用者（网络、音频或 Application）任务中直接改 UI。
    UiManager::GetInstance().DispatchUrgent([status]() {
        current_localized_status = status;
        HomePresenter::GetInstance().SetStatus(LocalizeStatus(status),
                                               status == Status::Standby);
    });
}

void SetStatusText(std::string text) {
    UiManager::GetInstance().DispatchUrgent([text = std::move(text)]() mutable {
        current_localized_status.reset();
        HomePresenter::GetInstance().SetStatus(std::move(text), false);
    });
}

void ShowNotification(Notification notification, int32_t duration_ms, std::string detail) {
    UiManager::GetInstance().Dispatch(
        [notification, duration_ms, detail = std::move(detail)]() mutable {
            auto text = LocalizeNotification(notification, detail);
            if (!text.empty()) {
                ShowNotificationOnDisplay(std::move(text), duration_ms);
            }
        });
}

void ShowNotification(std::string text, int32_t duration_ms) {
    UiManager::GetInstance().Dispatch([text = std::move(text), duration_ms]() mutable {
        ShowNotificationOnDisplay(std::move(text), duration_ms);
    });
}

void ShowToast(std::string text, int32_t duration_ms) {
    UiManager::GetInstance().Dispatch([text = std::move(text), duration_ms]() mutable {
        auto& manager = UiManager::GetInstance();
        if (manager.lvgl_display() != nullptr) {
            Toast::GetInstance().Show(text, duration_ms);
        } else if (auto* display = manager.display(); display != nullptr) {
            display->ShowNotification(text, duration_ms);
        }
    });
}

void DismissToast() {
    UiManager::GetInstance().Dispatch([]() {
        if (UiManager::GetInstance().lvgl_display() != nullptr) {
            Toast::GetInstance().Dismiss();
        }
    });
}

void SetChatMessage(std::string role, std::string content) {
    UiManager::GetInstance().Dispatch(
        [role = std::move(role), content = std::move(content)]() mutable {
            if (UiManager::GetInstance().lvgl_display() != nullptr) {
                HomePresenter::GetInstance().SetChatMessage(std::move(role), std::move(content));
            } else if (auto* display = UiManager::GetInstance().display(); display != nullptr) {
                display->SetChatMessage(role.c_str(), content.c_str());
            }
        });
}

void SetVoicePrompt(std::string text) {
    UiManager::GetInstance().DispatchUrgent([text = std::move(text)]() mutable {
        HomePresenter::GetInstance().SetText(std::move(text));
    });
}

void SetWakeWord(std::string wake_word) {
    UiManager::GetInstance().Dispatch([wake_word = std::move(wake_word)]() mutable {
        HomePresenter::GetInstance().SetWakeWord(std::move(wake_word));
    });
}

void SetWifiIcon(std::string icon) {
    UiManager::GetInstance().Dispatch([icon = std::move(icon)]() mutable {
        HomePresenter::GetInstance().SetWifiIcon(std::move(icon));
    });
}

void SetTfCardPresent(bool present) {
    UiManager::GetInstance().Dispatch([present]() {
        HomePresenter::GetInstance().SetTfCardPresent(present);
    });
}

void SetClockText(std::string text) {
    UiManager::GetInstance().Dispatch([text = std::move(text)]() mutable {
        HomePresenter::GetInstance().SetTime(std::move(text));
    });
}

void SetEmotion(std::string emotion) {
    if (ShouldIgnoreStandbyEmotion(emotion)) {
        return;
    }
    UiManager::GetInstance().DispatchUrgent([emotion = std::move(emotion)]() mutable {
        HomePresenter::GetInstance().SetEmotion(std::move(emotion));
    });
}

void ShowEmotionOnce(std::string emotion) {
    if (ShouldIgnoreStandbyEmotion(emotion)) {
        return;
    }
    UiManager::GetInstance().Dispatch([emotion = std::move(emotion)]() mutable {
        if (UiManager::GetInstance().lvgl_display() != nullptr) {
            HomePresenter::GetInstance().ShowEmotionOnce(std::move(emotion));
        } else if (auto* display = UiManager::GetInstance().display(); display != nullptr) {
            display->ShowEmotionOnce(emotion.c_str());
        }
    });
}

void SetMusicTrack(MusicTrack track) {
    UiManager::GetInstance().Dispatch([track = std::move(track)]() mutable {
        HomePresenter::GetInstance().SetMusicTrack(std::move(track));
    });
}

void SetMusicPaused(bool paused) {
    UiManager::GetInstance().Dispatch([paused]() {
        HomePresenter::GetInstance().SetMusicPaused(paused);
    });
}

void SetAlarmActive(bool active) {
    UiManager::GetInstance().Dispatch([active]() {
        HomePresenter::GetInstance().SetAlarmActive(active);
    });
}

void SetChargingAnimationReady(bool ready) {
    UiManager::GetInstance().Dispatch([ready]() {
        HomePresenter::GetInstance().SetChargingAnimationReady(ready);
    });
}

void ShowAlarmRing(const AlarmSnapshot& alarm) {
    UiManager::GetInstance().Dispatch([alarm]() {
        if (UiManager::GetInstance().lvgl_display() == nullptr) {
            ShowNotificationOnDisplay(
                alarm.spec.text.empty() ? "闹钟响了" : alarm.spec.text, 15000);
            return;
        }
        AlarmRingPresenter::GetInstance().ShowAlarm(alarm);
    });
}

void DismissAlarmRing() {
    UiManager::GetInstance().Dispatch([]() {
        AlarmRingPresenter::GetInstance().Dismiss();
    });
}

void ShowAlarmSuccess(const AlarmSnapshot& alarm, bool newly_created) {
    UiManager::GetInstance().Dispatch([alarm, newly_created]() {
        auto& manager = UiManager::GetInstance();
        if (manager.lvgl_display() == nullptr) {
            ShowNotificationOnDisplay("闹钟设置成功", 3000);
            return;
        }
        if (manager.TopScreenId() == UI_PAGE_ALARM_RING) {
            if (newly_created) {
                Toast::GetInstance().Show("新闹钟创建成功", 3000);
            } else {
                HomePresenter::GetInstance().ShowNotification("闹钟设置成功", 3000);
            }
            return;
        }
        AlarmSuccessPresenter::GetInstance().ShowAlarm(alarm);
    });
}

void UpdateStatusBar(bool update_all) {
    UiManager::GetInstance().Dispatch([update_all]() {
        UpdateStatusBarOnUi(update_all);
    });
}

void DismissOverlays() {
    UiManager::GetInstance().Dispatch([]() {
#ifdef CONFIG_LSPLATFORM
        if (UiManager::GetInstance().lvgl_display() != nullptr) {
            if (UiManager::GetInstance().TopScreenId() == UI_PAGE_QRCODE) {
                UiManager::GetInstance().NavigateHome();
            } else {
                HomePresenter::GetInstance().ClearImage();
            }
        } else if (auto* display = UiManager::GetInstance().display(); display != nullptr) {
            display->DismissImage();
            display->DismissActivation();
        }
#endif
    });
}

void ShowVolume(int32_t volume, bool percent) {
    auto text = std::string(Lang::Strings::VOLUME) + std::to_string(volume);
    if (percent) {
        text += "%";
    }
    ShowVolumeText(std::move(text));
}

void ShowMaxVolume() {
    ShowVolumeText(Lang::Strings::MAX_VOLUME);
}

void ShowMuted() {
    ShowVolumeText(Lang::Strings::MUTED);
}

void ToggleLanguage() {
    UiManager::GetInstance().Dispatch([]() {
        Lang::ToggleLanguage();
        KvUser::SetUiLanguage(Lang::GetLanguage() == Lang::Language::English ? 1 : 0);
        {
            Settings settings(kUiSettingsNamespace, true);
            settings.SetString(kLanguageSettingsKey, Lang::GetLanguageCode());
        }
        if (current_localized_status.has_value()) {
            HomePresenter::GetInstance().SetStatus(
                LocalizeStatus(*current_localized_status),
                *current_localized_status == Status::Standby);
        }
        UpdateStatusBarOnUi(true);
        ShowNotificationOnDisplay(Lang::Strings::LANGUAGE_SWITCHED, 3000);
        ESP_LOGI(kTag, "Display language switched to %s", Lang::GetLanguageCode());
    });
}

void SetLanguageMode(int32_t mode) {
    if (mode != 0 && mode != 1) {
        ESP_LOGW(kTag, "Ignoring unsupported language mode: %ld", static_cast<long>(mode));
        return;
    }
    UiManager::GetInstance().Dispatch([mode]() {
        const auto language = mode == 1 ? Lang::Language::English : Lang::Language::Primary;
        Lang::SetLanguage(language);
        Settings settings(kUiSettingsNamespace, true);
        settings.SetString(kLanguageSettingsKey, Lang::GetLanguageCode());
        if (current_localized_status.has_value()) {
            HomePresenter::GetInstance().SetStatus(
                LocalizeStatus(*current_localized_status),
                *current_localized_status == Status::Standby);
        }
        UpdateStatusBarOnUi(true);
        ShowNotificationOnDisplay(Lang::Strings::LANGUAGE_SWITCHED, 3000);
        ESP_LOGI(kTag, "Display language set to %s", Lang::GetLanguageCode());
    });
}

void SetLocalizedText(TextKey key) {
    UiManager::GetInstance().Dispatch([key]() {
        current_localized_status.reset();
        switch (key) {
            case TextKey::Error:
                HomePresenter::GetInstance().SetStatus(Lang::Strings::ERROR);
                break;
            case TextKey::PleaseWait:
                HomePresenter::GetInstance().SetStatus(Lang::Strings::PLEASE_WAIT);
                break;
        }
    });
}

void ShowWifiConfigHint(std::string ssid, std::string url) {
    UiManager::GetInstance().Dispatch(
        [ssid = std::move(ssid), url = std::move(url)]() mutable {
            std::string hint = std::string(Lang::Strings::CONNECT_TO_HOTSPOT) + ssid;
            hint += Lang::Strings::ACCESS_VIA_BROWSER;
            hint += url;
            HomePresenter::GetInstance().SetStatus(Lang::Strings::WIFI_CONFIG_MODE);
            HomePresenter::GetInstance().SetEmotion("gear");
            HomePresenter::GetInstance().SetChatMessage("system", std::move(hint));
        });
}

void SetPowerSaveMode(bool enabled) {
    UiManager::GetInstance().Dispatch([enabled]() {
        if (UiManager::GetInstance().lvgl_display() != nullptr) {
            HomePresenter::GetInstance().SetPowerSaveMode(enabled);
        } else if (auto* display = UiManager::GetInstance().display(); display != nullptr) {
            display->SetPowerSaveMode(enabled);
        }
    });
}

void SetDuplexMode(DuplexMode mode) {
    UiManager::GetInstance().Dispatch([mode]() {
        HomePresenter::GetInstance().SetDuplexMode(mode);
    });
}

void SetDuplexVisible(bool visible) {
    UiManager::GetInstance().Dispatch([visible]() {
        HomePresenter::GetInstance().SetDuplexVisible(visible);
    });
}

void ShowUpgradeProgress(std::string target, int progress, size_t speed) {
    UiManager::GetInstance().Dispatch([target = std::move(target), progress, speed]() mutable {
        auto& manager = UiManager::GetInstance();
        if (manager.lvgl_display() != nullptr) {
            auto& presenter = UpgradePresenter::GetInstance();
            presenter.SetProgress(std::move(target), progress, speed);
            if (manager.TopScreenId() != UI_PAGE_UPGRADE) {
                manager.NavigateTo(UI_PAGE_UPGRADE);
            }
            return;
        }
        if (auto* display = UiManager::GetInstance().display(); display != nullptr) {
            char text[80];
            const int bounded = std::max(0, std::min(1000, progress));
            std::snprintf(text, sizeof(text), "%s %d.%d%%", target.c_str(),
                          bounded / 10, bounded % 10);
            display->SetChatMessage("system", text);
        }
    });
}

void ShowUpgradeResult(std::string message) {
    ESP_LOGI(kTag, "Showing OTA result: %s", message.c_str());
    UiManager::GetInstance().Dispatch([message = std::move(message)]() mutable {
        auto& manager = UiManager::GetInstance();
        if (manager.lvgl_display() != nullptr) {
            auto& presenter = UpgradePresenter::GetInstance();
            if (manager.TopScreenId() != UI_PAGE_UPGRADE) {
                manager.NavigateTo(UI_PAGE_UPGRADE);
            }
            presenter.SetResult(std::move(message));
        } else if (auto* display = manager.display(); display != nullptr) {
            display->SetChatMessage("system", message.c_str());
        }
    });
}

void ShowSdMusicSync(SdMusicSyncState state) {
    UiManager::GetInstance().Dispatch([state]() {
        auto& manager = UiManager::GetInstance();
        if (manager.lvgl_display() != nullptr) {
            SdMusicSyncPresenter::GetInstance().SetState(state);
            return;
        }
        if (auto* display = manager.display(); display != nullptr) {
            char text[96] = {};
            if (state.phase == SdMusicSyncPhase::kScanning) {
                std::snprintf(text, sizeof(text), "TF卡扫描中");
            } else if (state.phase == SdMusicSyncPhase::kUploading) {
                std::snprintf(text, sizeof(text), "TF卡文件 %lu/%lu",
                              static_cast<unsigned long>(state.uploaded_count),
                              static_cast<unsigned long>(state.total_count));
            } else if (state.phase == SdMusicSyncPhase::kSucceeded) {
                std::snprintf(text, sizeof(text), "TF卡处理完成");
            } else if (state.phase == SdMusicSyncPhase::kFailed) {
                std::snprintf(text, sizeof(text), "TF卡文件列表上报失败");
            }
            display->SetChatMessage("system", text);
        }
    });
}

void DismissSdMusicSync() {
    UiManager::GetInstance().Dispatch([]() {
        SdMusicSyncPresenter::GetInstance().Dismiss();
    });
}

void HandleSettingMessage(const cJSON* data) {
    if (data == nullptr) {
        return;
    }
    // 服务端回调返回后原始 cJSON 会失效，复制一份再交给 UI 任务统一解析。
    auto copy = std::shared_ptr<cJSON>(cJSON_Duplicate(data, true), cJSON_Delete);
    if (copy == nullptr) {
        ESP_LOGE(kTag, "Failed to copy UI setting message");
        return;
    }
    UiManager::GetInstance().Dispatch([copy = std::move(copy)]() {
        HomePresenter::GetInstance().HandleSettingMessage(copy.get());
        QrCodePresenter::GetInstance().HandleSettingMessage(copy.get());
    });
}

void ClearRoleConfigQrCode() {
    UiManager::GetInstance().Dispatch([]() {
        QrCodePresenter::GetInstance().ClearRoleConfigUrl();
    });
}

bool HasRoleConfigQrCode() {
    return QrCodePresenter::GetInstance().HasRoleConfigQrCode();
}

void ShowRoleConfigQrCode() {
    UiManager::GetInstance().Dispatch([]() {
        QrCodePresenter::GetInstance().SelectRoleConfig();
        UiManager::GetInstance().NavigateTo(UI_PAGE_QRCODE);
    });
}

void ShowQrCode(std::unique_ptr<LvglImage> image, std::string message) {
    if (image == nullptr) {
        return;
    }
    // std::function 要求闭包可复制，用共享 holder 跨任务传递唯一所有权。
    auto pending = std::make_shared<std::unique_ptr<LvglImage>>(std::move(image));
    UiManager::GetInstance().Dispatch(
        [pending = std::move(pending), message = std::move(message)]() mutable {
            QrCodePresenter::GetInstance().SetCustomQrCode(
                std::move(*pending), std::move(message));
            UiManager::GetInstance().NavigateTo(UI_PAGE_QRCODE);
        });
}

void DismissQrCode() {
    UiManager::GetInstance().Dispatch([]() {
        auto& manager = UiManager::GetInstance();
        if (manager.TopScreenId() == UI_PAGE_QRCODE) {
            manager.NavigateHome();
        }
    });
}

void ShowImage(std::unique_ptr<LvglImage> image) {
#ifdef CONFIG_LSPLATFORM
    if (image == nullptr) {
        return;
    }
    auto pending = std::make_shared<std::unique_ptr<LvglImage>>(std::move(image));
    UiManager::GetInstance().Dispatch([pending = std::move(pending)]() mutable {
        HomePresenter::GetInstance().SetImage(std::move(*pending));
    });
#else
    (void)image;
#endif
}

void ShowPreviewImage(std::unique_ptr<LvglImage> image) {
#ifdef CONFIG_LSPLATFORM
    if (image == nullptr) {
        return;
    }
    auto pending = std::make_shared<std::unique_ptr<LvglImage>>(std::move(image));
    UiManager::GetInstance().Dispatch([pending = std::move(pending)]() mutable {
        auto& manager = UiManager::GetInstance();
        if (manager.TopScreenId() == UI_PAGE_CAMERA_PREVIEW) {
            CameraPreviewPresenter::GetInstance().SetFrame(std::move(*pending));
        } else {
            HomePresenter::GetInstance().SetImage(std::move(*pending));
        }
    });
#else
    (void)image;
#endif
}

void ShowCameraPreview(CameraPreviewPhase phase, std::string detail) {
    UiManager::GetInstance().Dispatch(
        [phase, detail = std::move(detail)]() mutable {
            auto& manager = UiManager::GetInstance();
#ifdef CONFIG_LSPLATFORM
            HomePresenter::GetInstance().ClearImage();
#endif
            CameraPreviewPresenter::GetInstance().SetState(
                phase, std::move(detail));
            manager.NavigateTo(UI_PAGE_CAMERA_PREVIEW);
        });
}

void FreezeCameraPreviewToHome(std::string status) {
    UiManager::GetInstance().Dispatch(
        [status = std::move(status)]() mutable {
            auto& manager = UiManager::GetInstance();
            auto frame = CameraPreviewPresenter::GetInstance().TakeFrame();
            const bool has_frame = frame != nullptr;
#ifdef CONFIG_LSPLATFORM
            if (has_frame) {
                HomePresenter::GetInstance().SetImage(std::move(frame));
            }
#else
            (void)frame;
#endif
            current_localized_status.reset();
            HomePresenter::GetInstance().SetStatus(std::move(status), false);
            const int previous_page = manager.TopScreenId();
            // 拍照冻结后的业务目标固定是 Home。无条件导航可避免顶层页面 ID
            // 短暂变化时照片和字幕只更新到后台的 Home 页面。
            const int navigate_result = manager.NavigateHome();
            ESP_LOGI(kTag,
                     "冻结照片并返回 Home: previous=%d, result=%d, current=%d, frame=%d",
                     previous_page, navigate_result, manager.TopScreenId(),
                     has_frame ? 1 : 0);
        });
}

void SetHomeSubtitle(std::string text) {
    UiManager::GetInstance().Dispatch([text = std::move(text)]() mutable {
        HomePresenter::GetInstance().SetSubtitleText(std::move(text));
    });
}

void SetCameraPreviewState(CameraPreviewPhase phase, std::string detail) {
    UiManager::GetInstance().Dispatch(
        [phase, detail = std::move(detail)]() mutable {
            CameraPreviewPresenter::GetInstance().SetState(phase, std::move(detail));
        });
}

void DismissCameraPreview() {
    UiManager::GetInstance().Dispatch([]() {
        auto& manager = UiManager::GetInstance();
        CameraPreviewPresenter::GetInstance().ClearFrame();
        if (manager.TopScreenId() == UI_PAGE_CAMERA_PREVIEW) {
            manager.NavigateHome();
        }
    });
}

void DismissImage() {
    UiManager::GetInstance().Dispatch([]() {
#ifdef CONFIG_LSPLATFORM
        auto& manager = UiManager::GetInstance();
        if (manager.lvgl_display() != nullptr) {
            HomePresenter::GetInstance().ClearImage();
        } else if (auto* display = manager.display(); display != nullptr) {
            display->DismissImage();
        }
#endif
    });
}

bool IsHome() {
    auto& manager = UiManager::GetInstance();
    return manager.lvgl_display() == nullptr ||
           manager.TopScreenId() == UI_PAGE_HOME;
}

std::vector<std::string> GetEmotionNames() {
    auto* display = UiManager::GetInstance().display();
    auto* theme = display != nullptr
                      ? dynamic_cast<LvglTheme*>(display->GetTheme())
                      : nullptr;
    auto repository = theme != nullptr ? theme->emoji_repository() : nullptr;
    return repository != nullptr ? repository->GetNames()
                                 : std::vector<std::string>{};
}

void NavigateHome() {
    UiManager::GetInstance().Dispatch([]() {
        UiManager::GetInstance().NavigateHome();
    });
}

}  // Ui 命名空间
