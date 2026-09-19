#include "ui_app.h"

#include "board.h"
#include "display/display.h"
#include "system/device_status.h"
#include "shell/settings.h"
#include "shell/cmd/kv/kv_user.h"
#include "ui/i18n/lang_config.h"
#include "ui/screens/home/home_presenter.h"
#include "ui/screens/qrcode/qrcode_presenter.h"
#include "ui/screens/upgrade/upgrade_presenter.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"

#include <esp_log.h>

#include <string>
#include <cstdio>
#include <optional>
#include <utility>

#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
#include "resource/assets.h"
#include "lvgl_theme.h"
#include "ui/utils/emoji/emoji_repository.h"

#include <memory>
#endif

namespace {

constexpr char kTag[] = "UiApp";
constexpr char kUiSettingsNamespace[] = "ui";
constexpr char kLanguageSettingsKey[] = "language";
std::optional<UiApp::Status> current_localized_status;

std::string LocalizeStatus(UiApp::Status status) {
    switch (status) {
        case UiApp::Status::Initializing:
            return Lang::Strings::INITIALIZING;
        case UiApp::Status::ScanningWifi:
            return Lang::Strings::SCANNING_WIFI;
        case UiApp::Status::RegisteringNetwork:
            return Lang::Strings::REGISTERING_NETWORK;
        case UiApp::Status::DetectingModule:
            return Lang::Strings::DETECTING_MODULE;
        case UiApp::Status::CheckingNewVersion:
            return Lang::Strings::CHECKING_NEW_VERSION;
        case UiApp::Status::LoadingProtocol:
            return Lang::Strings::LOADING_PROTOCOL;
        case UiApp::Status::Standby:
            return Lang::Strings::STANDBY;
        case UiApp::Status::Connecting:
            return Lang::Strings::CONNECTING;
        case UiApp::Status::Listening:
            return Lang::Strings::LISTENING;
        case UiApp::Status::Speaking:
            return Lang::Strings::SPEAKING;
    }
    return Lang::Strings::INITIALIZING;
}

std::string LocalizeNotification(UiApp::Notification notification, const std::string& detail) {
    switch (notification) {
        case UiApp::Notification::ScanningWifi:
            return Lang::Strings::SCANNING_WIFI;
        case UiApp::Notification::ConnectingToNetwork:
            return std::string(Lang::Strings::CONNECT_TO) + detail + "...";
        case UiApp::Notification::ConnectedToNetwork:
            return std::string(Lang::Strings::CONNECTED_TO) + detail;
        case UiApp::Notification::SwitchTo4G:
            return Lang::Strings::SWITCH_TO_4G_NETWORK;
        case UiApp::Notification::SwitchToWifi:
            return Lang::Strings::SWITCH_TO_WIFI_NETWORK;
        case UiApp::Notification::EnteringWifiConfigMode:
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

namespace UiApp {

bool ReloadEmojiResources() {
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    auto* display = UiManager::GetInstance().display();
    if (display == nullptr) {
        return false;
    }
    DisplayLockGuard lock(display);
    auto& themes = LvglThemeManager::GetInstance();
    auto* dark = themes.GetTheme("dark");
    if (dark == nullptr || dark->emoji_repository() == nullptr) {
        return false;
    }
    return dark->emoji_repository()->LoadFromAssets(Assets::GetInstance().EmojiStore());
#else
    return true;
#endif
}

void SetBannerIdleProvider(std::function<bool()> provider) {
    HomePresenter::GetInstance().SetBannerIdleProvider(std::move(provider));
}

bool Initialize() {
    Settings settings(kUiSettingsNamespace);
    const auto saved_language = settings.GetString(kLanguageSettingsKey);
    const auto user_language = KvUser::GetUiLanguage();
    if (user_language == 0 || user_language == 1) {
        Lang::SetLanguage(user_language == 1 ? Lang::Language::English
                                             : Lang::Language::Primary);
    } else if (!saved_language.empty() && !Lang::SetLanguageByCode(saved_language.c_str())) {
        ESP_LOGW(kTag, "Ignoring unsupported saved language: %s", saved_language.c_str());
    }

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

#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    // Home 页面创建前先加载表情动画定义，避免页面打开后再解析 JSON。
    auto emoji_repository = std::make_shared<EmojiRepository>();
    if (emoji_repository->LoadFromAssets(Assets::GetInstance().EmojiStore())) {
        auto& themes = LvglThemeManager::GetInstance();
        if (auto* light = themes.GetTheme("light")) {
            light->set_emoji_repository(emoji_repository);
        }
        if (auto* dark = themes.GetTheme("dark")) {
            dark->set_emoji_repository(emoji_repository);
        }
    } else {
        ESP_LOGW(kTag, "表情动画资源加载失败，Home 页面将使用无表情回退");
    }
#endif

    if (ui.RegisterScreen(&HomePresenter::Screen()) != 0 ||
        ui.RegisterScreen(&QrCodePresenter::Screen()) != 0 ||
        ui.RegisterScreen(&UpgradePresenter::Screen()) != 0 ||
        ui.SetDefaultScreen(UI_PAGE_HOME) != 0) {
        return false;
    }
    return true;
}

void SetStatus(Status status) {
    UiManager::GetInstance().Dispatch([status]() {
        current_localized_status = status;
        HomePresenter::GetInstance().SetStatus(LocalizeStatus(status));
    });
}

void SetStatusText(std::string text) {
    UiManager::GetInstance().Dispatch([text = std::move(text)]() mutable {
        current_localized_status.reset();
        HomePresenter::GetInstance().SetStatus(std::move(text));
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
    UiManager::GetInstance().Dispatch([text = std::move(text)]() mutable {
        HomePresenter::GetInstance().SetText(std::move(text));
    });
}

void SetEmotion(std::string emotion) {
    if (ShouldIgnoreStandbyEmotion(emotion)) {
        return;
    }
    UiManager::GetInstance().Dispatch([emotion = std::move(emotion)]() mutable {
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

void UpdateStatusBar(bool update_all) {
    UiManager::GetInstance().Dispatch([update_all]() {
        if (auto* display = UiManager::GetInstance().display(); display != nullptr) {
            display->UpdateStatusBar(update_all);
        }
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
            HomePresenter::GetInstance().SetStatus(LocalizeStatus(*current_localized_status));
        }
        if (auto* display = UiManager::GetInstance().display(); display != nullptr) {
            display->UpdateStatusBar(true);
        }
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
            HomePresenter::GetInstance().SetStatus(LocalizeStatus(*current_localized_status));
        }
        if (auto* display = UiManager::GetInstance().display(); display != nullptr) {
            display->UpdateStatusBar(true);
        }
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
        } else if (auto* display = manager.display(); display != nullptr) {
            char text[80];
            std::snprintf(text, sizeof(text), "%s %d%%", target.c_str(), progress);
            display->SetChatMessage("system", text);
        }
    });
}

void NavigateToQrCode() {
    UiManager::GetInstance().Dispatch([]() {
        UiManager::GetInstance().NavigateTo(UI_PAGE_QRCODE);
    });
}

void NavigateHome() {
    UiManager::GetInstance().Dispatch([]() {
        UiManager::GetInstance().NavigateHome();
    });
}

}  // UiApp 命名空间
