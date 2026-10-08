#include "application.h"

#include "alarm/alarm_service.h"
#include "boards/common/board.h"
#include "camera/camera_service.h"
#include "system/system_reset.h"
#include "sd_card/sd_music_service.h"
#include "ui/ui.h"

#include <esp_log.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

namespace {
constexpr char kTag[] = "ApplicationButton";
// 设备尚未取得角色配置时，播放通用配网提示音。
constexpr uint16_t kProvisioningToneId = 70;
// 设备已取得角色配置时，播放设备配置二维码提示音。
constexpr uint16_t kDeviceConfigToneId = 104;
}

// 板级按键回调可能运行在非应用主任务中，这里只投递短击事件，
// 具体业务统一回到 Application 主任务串行执行。
void Application::HandleButtonClick(uint8_t click_count) {
    if (GetDeviceState() == kDeviceStateActivating) {
        ESP_LOGI(kTag, "激活期间忽略 %u 击按键",
                 static_cast<unsigned>(click_count));
        return;
    }
    Schedule([this, click_count]() {
        ProcessButtonClick(click_count);
    });
}

// 将板级长按回调切换到 Application 主任务，避免在按键回调中直接
// 操作闹钟、播放器和 OTA 状态。
void Application::HandleButtonLongPress() {
    if (GetDeviceState() == kDeviceStateActivating) {
        ESP_LOGI(kTag, "激活期间忽略长按按键");
        return;
    }
    Schedule([this]() {
        ProcessButtonLongPress();
    });
}

// 长按优先停止正在响铃的闹钟；如果正在等待固件升级确认，则跳过本次升级。
void Application::ProcessButtonLongPress() {
    const auto state = GetDeviceState();
    if (state == kDeviceStateActivating) {
        ESP_LOGI(kTag, "激活期间忽略长按按键");
        return;
    }
    if (state == kDeviceStateCheckingUpdate) {
        if (!HandleOtaConfirmation(false)) {
            ESP_LOGI(kTag, "OTA 检查期间忽略长按按键");
        }
        return;
    }
    if (AlarmService::GetInstance().IsRinging()) {
        ESP_LOGI(kTag, "Long press: stop alarm");
        AlarmService::GetInstance().Stop(true);
        return;
    }
    if (CameraService::GetInstance().Exit()) {
        ESP_LOGI(kTag, "Long press: exit camera preview");
        return;
    }
    HandleOtaConfirmation(false);
}

// 在应用主任务中处理短击次数，优先响应闹钟和 OTA 确认，
// 然后再执行各个击次对应的常规功能。
void Application::ProcessButtonClick(uint8_t click_count) {
    const auto state = GetDeviceState();
    if (state == kDeviceStateActivating) {
        ESP_LOGI(kTag, "激活期间忽略 %u 击按键",
                 static_cast<unsigned>(click_count));
        return;
    }
    if (state == kDeviceStateCheckingUpdate) {
        if (click_count != 1 || !HandleOtaConfirmation(true)) {
            ESP_LOGI(kTag, "OTA 检查期间忽略 %u 击按键",
                     static_cast<unsigned>(click_count));
        }
        return;
    }
    // 闹钟响铃时，任意短击只用于贪睡或停止闹钟，不再执行其他按键功能。
    if (AlarmService::GetInstance().IsRinging()) {
        ESP_LOGI(kTag, "Click: %s alarm", AlarmService::GetInstance().CanSnooze() ? "snooze" : "stop");
        if (AlarmService::GetInstance().CanSnooze()) {
            AlarmService::GetInstance().Snooze();
        } else {
            AlarmService::GetInstance().Stop(true);
        }
        return;
    }
    auto& camera_service = CameraService::GetInstance();
    if (camera_service.CancelVoiceMcpCountdown("用户通过按键取消拍照")) {
        return;
    }
    if (camera_service.IsActive()) {
        if (click_count == 1) {
            camera_service.Capture();
        } else {
            ESP_LOGI(kTag, "相机预览期间忽略 %u 击",
                     static_cast<unsigned>(click_count));
        }
        return;
    }
    if (SdMusicService::GetInstance().IsBusy()) {
        ESP_LOGI(kTag, "TF 卡文件同步期间忽略普通按键");
        return;
    }

    switch (click_count) {
        case 1:
            // 单击：非主页时先返回主页；启动期允许进入配网；
            // 其余情况用于开始或结束语音交互。
            if (!Ui::IsHome()) {
                Ui::NavigateHome();
                break;
            }
            if (GetDeviceState() == kDeviceStateStarting &&
                Board::GetInstance().EnterNetworkConfigMode()) {
                return;
            }
            ToggleChatState();
            break;
        case 2:
            // 双击始终是拍照入口；无摄像头板型或硬件初始化失败
            // 由 CameraService 统一提示“未检测到摄像头”。
            camera_service.StartButtonCapture();
            break;
        case 3: {
            // 三击：打开角色配置二维码；尚无角色配置时展示通用配网入口。
            const bool has_role_config_qrcode = Ui::HasRoleConfigQrCode();
            Ui::ShowRoleConfigQrCode();
            PlayTone(has_role_config_qrcode ? kDeviceConfigToneId : kProvisioningToneId);
            break;
        }
        case 4:
            // 四击：切换 UI 语言。
            Ui::ToggleLanguage();
            break;
        case 5:
            // 五击：清除已保存的 Wi-Fi，断开小聆并重新进入配网。
            ClearNetworkConfiguration();
            break;
        default:
            // 八击及以上：恢复出厂设置。六击和七击当前不定义功能。
            if (click_count >= 8) {
                StartFactoryReset();
            }
            break;
    }
}

// 停止当前 Wi-Fi 连接、清除已保存的网络配置，然后进入配网状态。
void Application::ClearNetworkConfiguration() {
    const auto state = GetDeviceState();
    if (state == kDeviceStateUpgrading || state == kDeviceStateFatalError) {
        ESP_LOGW(kTag, "Cannot clear network configuration in state %s",
                 DeviceStatus::GetStateName(state));
        return;
    }
    WifiManager::GetInstance().StopStation();
    SsidManager::GetInstance().Clear();
    if (!Board::GetInstance().EnterNetworkConfigMode()) {
        ESP_LOGW(kTag, "Current board does not support Wi-Fi configuration reset");
        return;
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        SetDeviceState(kDeviceStateIdle);
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
    }
    SetDeviceState(kDeviceStateWifiConfiguring);
    ResetXiaolingConn();
}

// 创建独立任务执行耗时的提示音和恢复出厂流程。
void Application::StartFactoryReset() {
    ESP_LOGW(kTag, "Detected 8 or more clicks; factory reset requested");
    if (xTaskCreate([](void* arg) {
            static_cast<Application*>(arg)->FactoryResetTask();
        }, "factory_reset", 4096, this, 5, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create factory reset task");
    }
}

// 停止当前播放，完整播放恢复出厂提示音，随后清除设备数据并重启。
void Application::FactoryResetTask() {
    audio_player_.Stop(PlaybackEndReason::kCancelled);
    const auto session_id = audio_player_.PlayTone(103);
    for (int i = 0; session_id != 0 && i < 50 && !audio_player_.IsPlaying(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (session_id != 0 && audio_player_.IsPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    SystemReset::ResetToFactory();
    vTaskDelete(nullptr);
}
