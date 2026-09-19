#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

enum class DuplexMode;

namespace UiApp {

enum class Status : int32_t {
    Initializing,
    ScanningWifi,
    RegisteringNetwork,
    DetectingModule,
    CheckingNewVersion,
    LoadingProtocol,
    Standby,
    Connecting,
    Listening,
    Speaking,
};

enum class Notification : int32_t {
    ScanningWifi,
    ConnectingToNetwork,
    ConnectedToNetwork,
    SwitchTo4G,
    SwitchToWifi,
    EnteringWifiConfigMode,
};

enum class TextKey : int32_t {
    Error,
    PleaseWait,
};

bool Initialize();
// 资源分区更新后重新加载表情动画定义。
bool ReloadEmojiResources();
// 由 application 注入 Banner 是否允许轮换的设备状态。
void SetBannerIdleProvider(std::function<bool()> provider);

// 线程安全的 UI 业务入口：所有操作都由 UiManager 调度到 Application 主任务。
void SetStatus(Status status);
void SetStatusText(std::string text);
void ShowNotification(Notification notification, int32_t duration_ms = 3000,
                      std::string detail = {});
void ShowNotification(std::string text, int32_t duration_ms = 3000);
void SetChatMessage(std::string role, std::string content);
void SetVoicePrompt(std::string text);
void SetEmotion(std::string emotion);
void ShowEmotionOnce(std::string emotion);
void UpdateStatusBar(bool update_all = false);
void DismissOverlays();
void ShowVolume(int32_t volume, bool percent = false);
void ShowMaxVolume();
void ShowMuted();
// 切换主语言/英语，持久化选择并立即刷新当前状态文案。
void ToggleLanguage();
void SetLocalizedText(TextKey key);
void ShowWifiConfigHint(std::string ssid, std::string url);
void SetPowerSaveMode(bool enabled);
void SetDuplexMode(DuplexMode mode);
void ShowUpgradeProgress(std::string target, int progress, size_t speed = 0);
void NavigateToQrCode();
void NavigateHome();

}  // UiApp 命名空间
