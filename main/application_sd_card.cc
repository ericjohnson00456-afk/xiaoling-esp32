#include "application.h"

#include "board.h"
#include "sd_card/sd_card_manager.h"

#include <esp_log.h>

#define TAG "Application"

void Application::InitializeSdCard() {
    const auto* config = Board::GetInstance().GetSdCardConfig();
    if (config == nullptr) {
        return;
    }

    auto& manager = SdCardManager::GetInstance();
    const esp_err_t ret = manager.StartMonitoring(
        *config, [&manager](SdCardEvent event) {
            if (event == SdCardEvent::kInserted) {
                return;
            }
            const bool stopped = Application::GetInstance()
                                     .GetAudioPlayer()
                                     .StopLocalFilePlayback();
            if (!stopped) {
                ESP_LOGW(TAG, "拔卡后停止本地文件播放超时");
            }
            const esp_err_t unmount_ret = manager.Deinitialize();
            if (unmount_ret != ESP_OK) {
                ESP_LOGW(TAG, "拔卡后卸载失败: %s",
                         esp_err_to_name(unmount_ret));
            }
        });
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "启动 SD 卡轮询监测失败: %s", esp_err_to_name(ret));
    }
}
