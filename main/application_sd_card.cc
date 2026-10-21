#include "application.h"

#include "board.h"
#include "audio/wake_audio_recorder.h"
#include "sd_card/sd_card_manager.h"
#include "sd_card/sd_music_service.h"
#include "xiaoling/conn/xiaoling_conn.h"
#include "xiaoling/conn/xiaoling_config.h"

#include <esp_log.h>

#define TAG "Application"

void Application::InitializeSdCard() {
    const auto* config = Board::GetInstance().GetSdCardConfig();
    if (config == nullptr) {
        return;
    }

    auto& manager = SdCardManager::GetInstance();
    auto& music_service = SdMusicService::GetInstance();
    // Capture the cached environment host before SD workers move to PSRAM
    // stacks, keeping the worker context independent from configuration state.
    const std::string host = XiaolingConfig::GetHost();
    music_service.Initialize(host, [this]() {
        std::lock_guard<std::mutex> lock(xiaoling_conn_mutex_);
        return xiaoling_conn_ != nullptr
                   ? xiaoling_conn_->GetHttpAuthorization()
                   : std::string{};
    });
    const esp_err_t ret = manager.StartMonitoring(
        *config, [&manager, &music_service](SdCardEvent event) {
            if (event == SdCardEvent::kInserted) {
                WakeAudioRecorder::GetInstance().SetCardAvailable(true);
                music_service.OnCardInserted();
                return;
            }
            if (event == SdCardEvent::kUnsupportedFileSystem) {
                music_service.OnUnsupportedFileSystem();
                return;
            }
            WakeAudioRecorder::GetInstance().SetCardAvailable(false);
            music_service.OnCardRemoved();
            Application::GetInstance().Schedule([]() {
                Application::GetInstance()
                    .GetIntentManager()
                    .Music()
                    .OnTfCardRemoved();
            });
            if (!music_service.WaitForFileAccessToStop(12000)) {
                ESP_LOGW(TAG, "拔卡后等待 TF 卡扫描停止超时");
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
