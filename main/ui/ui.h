#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

enum class DuplexMode;
class LvglImage;
struct cJSON;
struct AlarmSnapshot;

namespace Ui {

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

/*
 * 初始化顺序：
 * 1. 读取语言配置；
 * 2. 从 Board 取得已经创建好的 Display，并交给 UiManager；
 * 3. 注册页面并指定首页；
 * 4. 启动 UI 专用任务。
 *
 * 初始化完成后，业务模块只调用下面的 Ui::XXX() 接口发送界面命令。
 */
bool Initialize();

// 线程安全的 UI 业务入口：所有界面更新都由 UI 专用任务串行执行。
void SetStatus(Status status);
void SetStatusText(std::string text);
void ShowNotification(Notification notification, int32_t duration_ms = 3000,
                      std::string detail = {});
void ShowNotification(std::string text, int32_t duration_ms = 3000);
// 与页面无关的临时浮层提示。
void ShowToast(std::string text, int32_t duration_ms = 3000);
void DismissToast();
void SetChatMessage(std::string role, std::string content);
void SetVoicePrompt(std::string text);
void SetWakeWord(std::string wake_word);
void SetWifiIcon(std::string icon);
void SetClockText(std::string text);
void SetEmotion(std::string emotion);
void ShowEmotionOnce(std::string emotion);
// 在 Home 页电池图标旁显示倒计时运行标记。
void SetAlarmActive(bool active);
void ShowAlarmRing(const AlarmSnapshot& alarm);
void DismissAlarmRing();
void ShowAlarmSuccess(const AlarmSnapshot& alarm, bool newly_created = false);
void UpdateStatusBar(bool update_all = false);
void DismissOverlays();
void ShowVolume(int32_t volume, bool percent = false);
void ShowMaxVolume();
void ShowMuted();
// 切换主语言/英语，持久化选择并立即刷新当前状态文案。
void ToggleLanguage();
// 按用户 KV 值设置语言：0 为主语言，1 为英语。
void SetLanguageMode(int32_t mode);
void SetLocalizedText(TextKey key);
void ShowWifiConfigHint(std::string ssid, std::string url);
void SetPowerSaveMode(bool enabled);
void SetDuplexMode(DuplexMode mode);
void SetDuplexVisible(bool visible);
void ShowUpgradeProgress(std::string target, int progress, size_t speed = 0);
void ShowUpgradeResult(std::string message);

// 将服务端 UI 配置中的 Banner 和角色二维码更新交给 UI 模块处理。
void HandleSettingMessage(const cJSON* data);
void ClearRoleConfigQrCode();
// 查询接口同步返回；显示接口仍由 UI 任务异步执行。
bool HasRoleConfigQrCode();
void ShowRoleConfigQrCode();
void ShowQrCode(std::unique_ptr<LvglImage> image, std::string message);
void DismissQrCode();
void ShowImage(std::unique_ptr<LvglImage> image);
void DismissImage();

// 只返回 UI 模块自身的只读状态，不向外暴露 Manager 或 Presenter。
bool IsHome();
std::vector<std::string> GetEmotionNames();
void NavigateHome();

}  // Ui 命名空间
