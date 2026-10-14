#include "application.h"
#include "board.h"
#include "display/display.h"
#include "system/system_info.h"
#include "codecs/audio_codec.h"
#include "xiaoling/conn/xiaoling_conn.h"
#include "xiaoling/conn/xiaoling_conn_activation.h"
#include "ui/i18n/lang_config.h"
#include "mcp_server.h"
#include "resource/resource.h"
#include "system/settings.h"
#include "serial_shell.h"
#include "ui/ui.h"
#include "shell/cmd/kv/kv_user.h"
#include "event/app_event.h"
#include "sd_card/sd_music_service.h"
#include "camera/camera_service.h"
#include "xiaoling/session/voice_session_epoch.h"
#include "player/types/tts_session_guard.h"

#ifdef CONFIG_LSPLATFORM
#include "image_fetcher.h"
#endif  // 小聆平台

#include <cstring>
#include <algorithm>
#include <memory>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <font_awesome.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

static constexpr EventBits_t kAllMainEvents =
    MAIN_EVENT_SCHEDULE |
    MAIN_EVENT_CLOCK_TICK |
    MAIN_EVENT_ERROR |
    MAIN_EVENT_OTA_COMPLETED |
    MAIN_EVENT_NETWORK_CONNECTED |
    MAIN_EVENT_NETWORK_DISCONNECTED |
    MAIN_EVENT_TOGGLE_CHAT |
    MAIN_EVENT_START_LISTENING |
    MAIN_EVENT_STOP_LISTENING |
    MAIN_EVENT_STATE_CHANGED;

Application::Application() {
    event_group_ = xEventGroupCreate();

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

}

Application::~Application() {
    for (const auto subscription_id : audio_event_subscription_ids_) {
        AppEvent::Unsubscribe(subscription_id);
    }
    for (const auto subscription_id : playback_event_subscription_ids_) {
        AppEvent::Unsubscribe(subscription_id);
    }
    for (const auto subscription_id : playback_output_event_subscription_ids_) {
        AppEvent::Unsubscribe(subscription_id);
    }
    if (iat_subscription_id_ != 0) {
        AppEvent::Unsubscribe(iat_subscription_id_);
    }
    if (iat_update_subscription_id_ != 0) {
        AppEvent::Unsubscribe(iat_update_subscription_id_);
    }
    if (iat_end_subscription_id_ != 0) {
        AppEvent::Unsubscribe(iat_end_subscription_id_);
    }
    if (tts_url_subscription_id_ != 0) {
        AppEvent::Unsubscribe(tts_url_subscription_id_);
    }
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::HandlePlaybackEvent(const PlaybackEvent& event) {
    if (event.channel == PlaybackChannel::kMusic) {
        if ((event.state == PlaybackState::kCompleted ||
             event.state == PlaybackState::kStopped ||
             event.state == PlaybackState::kFailed) &&
            !audio_player_.HasMusicList() &&
            intent_manager_.Contains(PlaybackIntentId::kMusic)) {
            ESP_LOGI(TAG, "音乐播放列表已结束，退出音乐事件");
            intent_manager_.Music().Exit();
        }
        return;
    }
    if (event.channel == PlaybackChannel::kPrompt) {
        if (event.state == PlaybackState::kCompleted ||
            event.state == PlaybackState::kStopped ||
            event.state == PlaybackState::kFailed) {
            const uint32_t session_id = event.session_id;
            // 播放器事件运行在 AppEvent 任务中，Intent 栈操作统一投递到
            // Application 主任务，避免与按键或其他业务入口并发修改。
            Schedule([this, session_id]() {
                uint32_t expected = session_id;
                if (!prompt_tone_session_id_.compare_exchange_strong(expected, 0)) {
                    return;
                }
                intent_manager_.Pop(PlaybackIntentId::kPromptTone);
                ESP_LOGI(TAG, "通用提示音结束，释放提示音事件: session=%lu",
                         static_cast<unsigned long>(session_id));
            });
        }
        return;
    }
    if (event.channel != PlaybackChannel::kTts) {
        return;
    }
    if (pending_tts_session_id_ != 0 && pending_tts_session_id_ != event.session_id) {
        return;
    }
    if (event.state == PlaybackState::kStreamReady) {
        pending_tts_session_id_ = event.session_id;
        AppEvent::PublishScoped(AppEvent::Id::XiaolingTtsStreamReady, nullptr, 0,
            Xiaoling::voice_session_epoch, Xiaoling::voice_session_epoch.Current());
        return;
    }
    if (event.state == PlaybackState::kPlaying) {
        pending_tts_session_id_ = 0;
        active_tts_session_id_ = event.session_id;
        AppEvent::PublishScoped(AppEvent::Id::XiaolingTtsPlaying, nullptr, 0,
            Xiaoling::voice_session_epoch, Xiaoling::voice_session_epoch.Current());
        session_controller_.StartTts(event.session_id);
        // kPlaying 只表示播放器已进入播放态，仍需等待 DAC 首个 PCM 输出。
        if (!exit_requested_.load() &&
            tts_waiting_for_playback_.load() &&
            tts_output_started_.exchange(false) &&
            tts_waiting_for_playback_.exchange(false) &&
            (GetDeviceState() == kDeviceStateIdle ||
             GetDeviceState() == kDeviceStateListening)) {
            SetDeviceState(kDeviceStateSpeaking);
        }
        return;
    }
    if (event.state != PlaybackState::kCompleted &&
        event.state != PlaybackState::kStopped &&
        event.state != PlaybackState::kFailed) {
        return;
    }
    const bool active_session = active_tts_session_id_ == event.session_id;
    if (!IsCurrentTtsSession(event.session_id, active_tts_session_id_, pending_tts_session_id_)) {
        ESP_LOGI(TAG,
                 "忽略旧 TTS 终态: session=%lu, active=%lu, pending=%lu, state=%u",
                 static_cast<unsigned long>(event.session_id),
                 static_cast<unsigned long>(active_tts_session_id_),
                 static_cast<unsigned long>(pending_tts_session_id_),
                 static_cast<unsigned>(event.state));
        return;
    }
    // 确认 session 后才清标志；旧终态不能清掉新 TTS 的待播放状态。
    tts_waiting_for_playback_.store(false);
    tts_output_started_.store(false);
    const char* reason_text = "interrupted";
    if (event.reason == PlaybackEndReason::kNaturalEnd) {
        reason_text = "natural";
    } else if (event.reason == PlaybackEndReason::kFirstPcmTimeout) {
        reason_text = "first_pcm_timeout";
        ESP_LOGW(TAG, "TTS 首帧 PCM 超时: session=%lu",
                 static_cast<unsigned long>(event.session_id));
    } else if (event.reason == PlaybackEndReason::kPcmStallTimeout) {
        reason_text = "pcm_stall_timeout";
        ESP_LOGW(TAG, "TTS 播放中 PCM 断流超时: session=%lu",
                 static_cast<unsigned long>(event.session_id));
    }
    AppEvent::PublishTextScoped(AppEvent::Id::XiaolingTtsStopped, reason_text,
        Xiaoling::voice_session_epoch, Xiaoling::voice_session_epoch.Current());
    pending_tts_session_id_ = 0;
    active_tts_session_id_ = 0;
    if (active_session) {
        session_controller_.EndTts(event.session_id);
    }
    if (CameraService::GetInstance().OwnsStandaloneTtsLifecycle()) {
        // 按键拍照 TTS 是一次性事务，由拍照控制器负责收尾；若此处按普通
        // 半双工会话重启上行，会遗留语音会话并吞掉下一次照片识别结果。
        ESP_LOGI(TAG, "拍照 TTS 已结束，跳过半双工上行重启");
        return;
    }
    if (exit_requested_.load()) {
        // Exit 结束本轮上报，但不阻止用户通过新的唤醒打断告别 TTS。
        ESP_LOGI(TAG, "退出请求：TTS 播放结束，恢复唤醒检测但保持待命");
        if (GetDeviceState() != kDeviceStateIdle) {
            SetDeviceState(kDeviceStateIdle);
        }
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.EnableWakeWordDetection(true);
        exit_requested_.store(false);
        return;
    }
    if (intent_manager_.HasUnpausedMusic()) {
        // 音乐仍在栈中时，TTS 结束就是退出本轮录音并恢复音乐的屏障。
        ESP_LOGI(TAG, "TTS 播放结束，退出语音会话并恢复音乐");
        iat_utterance_active_ = false;
        audio_service_.EnableAudioUpload(false);
        session_controller_.EndSession();
        if (xiaoling_conn_) {
            xiaoling_conn_->EndSession();
        }
        if (GetDeviceState() != kDeviceStateIdle) {
            SetDeviceState(kDeviceStateIdle);
        }
        intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
        return;
    }
    if (GetDeviceState() == kDeviceStateSpeaking) {
        if (!exit_requested_.load() && session_controller_.KeepListeningDuringTts()) {
            SetDeviceState(kDeviceStateListening);
        } else if (!exit_requested_.load() &&
                   event.state == PlaybackState::kCompleted &&
                   session_controller_.policy().mode == DuplexMode::kHalfDuplex) {
            ESP_LOGI(TAG, "半双工 TTS 播放完成，重新开始上行");
            SetDeviceState(kDeviceStateListening);
        } else {
            SetDeviceState(kDeviceStateIdle);
        }
    }
}

void Application::HandleVadChangedEvent() {
    // 先释放 pending；若此时又发生变化，AudioService 回调会补发下一次通知。
    vad_event_pending_.store(false);
    if (GetDeviceState() == kDeviceStateListening) {
        Board::GetInstance().GetLed()->OnStateChanged();
    }
}

void Application::HandlePlayerOutputStartedEvent(
        const PlaybackOutputEvent& event) {
    // tone、music 和 alarm 也会经过同一个物理输出任务，不能当成 TTS。
    if (event.channel != PlaybackChannel::kTts) {
        return;
    }
    if (!IsCurrentTtsSession(event.session_id, active_tts_session_id_, pending_tts_session_id_)) {
        // 暂存 URL 在提示音结束后才分配 session，首个 PCM 可早于 Playing 事件。
        if (active_tts_session_id_ == 0 && pending_tts_session_id_ == 0 &&
            tts_waiting_for_playback_.load()) pending_tts_session_id_ = event.session_id;
        else return;
    }

    ESP_LOGI(TAG,
             "收到 TTS 扬声器首帧: session=%lu, waiting=%d, active=%lu, pending=%lu",
             static_cast<unsigned long>(event.session_id),
             tts_waiting_for_playback_.load() ? 1 : 0,
             static_cast<unsigned long>(active_tts_session_id_),
             static_cast<unsigned long>(pending_tts_session_id_));
    if (!tts_waiting_for_playback_.load()) {
        ESP_LOGW(TAG, "TTS 首帧未进入字幕时钟：当前没有待播放 URL");
        return;
    }

    tts_output_started_.store(true);
    AppEvent::PublishScoped(AppEvent::Id::XiaolingTtsStarted, nullptr, 0,
        Xiaoling::voice_session_epoch, Xiaoling::voice_session_epoch.Current());
    if (!exit_requested_.load() && active_tts_session_id_ != 0 &&
        tts_output_started_.exchange(false) &&
        tts_waiting_for_playback_.exchange(false) &&
        (GetDeviceState() == kDeviceStateIdle ||
         GetDeviceState() == kDeviceStateListening)) {
        SetDeviceState(kDeviceStateSpeaking);
    }
}

void Application::HandlePlayerOutputProgressEvent(
        const PlaybackOutputEvent& event) {
    if (event.session_id == 0) {
        return;
    }
    if (event.channel != PlaybackChannel::kTts) {
        return;
    }
    if (!IsCurrentTtsSession(event.session_id, active_tts_session_id_, pending_tts_session_id_)) return;
    AppEvent::PublishTextScoped(AppEvent::Id::XiaolingTtsOutputProgress,
        std::to_string(event.audio_clock_ms), Xiaoling::voice_session_epoch,
        Xiaoling::voice_session_epoch.Current());
}

void Application::Initialize() {
    // 业务事件在独立任务中串行分发。
    if (!AppEvent::Initialize()) {
        ESP_LOGE(TAG, "应用事件循环初始化失败");
    }

    // 挂载 ROMFS 格式资源。
    if (!Resource::Initialize()) {
        ESP_LOGE(TAG, "One or more required ROMFS resources are unavailable");
    }

    // 板型初始化。
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // 只有提供 SPI SD 卡配置的版型才会启动挂载和热插拔监测。
    InitializeSdCard();

    // UI 模块自行创建任务并串行处理界面更新，Application 只发送状态。
    if (!Ui::Initialize()) {
        ESP_LOGE(TAG, "Home 页面初始化失败");
    }
    // 切换为 AI 图标。
    Ui::SetEmotion("gear");
    // 显示开发板名称和版本信息。
    Ui::SetChatMessage("system", SystemInfo::GetUserAgent());

    // 启动串口调试终端。
    if (!SerialShell::GetInstance().Initialize()) {
        ESP_LOGW(TAG, "串口 Shell 初始化失败");
    }

    // 构造一次本地 MCP 工具表；WebSocket 首连和重连时直接复用。
    McpServer::GetInstance().Initialize();

    // 音频模块内部按固定顺序完成订阅、回调绑定和任务启动。
    InitializeAudio();

    // 闹钟的恢复、播放、时钟及云端同步钩子统一在 application_alarm.cc 安装。
    InitializeAlarm();

    CameraService::GetInstance().Initialize(*this);

    // 注册设备状态变化监听器。
    DeviceStatus::AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // 启动时钟定时器，用于更新状态栏。
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // 注册网络事件回调，用于更新 UI 和处理网络状态。
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Scanning:
                Ui::ShowNotification(Ui::Notification::ScanningWifi, 30000);
                network_connected_ = false;
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // 蜂窝网络正在注册，暂时还没有运营商信息。
                    Ui::SetStatus(Ui::Status::RegisteringNetwork);
                } else {
                    // Wi-Fi，或已经取得运营商信息的蜂窝网络。
                    Ui::ShowNotification(Ui::Notification::ConnectingToNetwork, 30000, data);
                }
                break;
            }
            case NetworkEvent::Connected: {
                Ui::ShowNotification(Ui::Notification::ConnectedToNetwork, 30000, data);
                network_connected_ = true;
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                network_connected_ = false;
                Ui::ClearRoleConfigQrCode();
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // 进入 Wi-Fi 配网模式由 WifiBoard 内部处理。
                break;
            case NetworkEvent::WifiConfigModeExit:
                // 退出 Wi-Fi 配网模式由 WifiBoard 内部处理。
                break;
            // 蜂窝网络模块专用事件。
            case NetworkEvent::ModemDetecting:
                Ui::SetStatus(Ui::Status::DetectingModule);
                break;
            case NetworkEvent::ModemErrorNoSim:
                ESP_LOGW(TAG, "[%s] %s: %s", "triangle_exclamation",
                         Lang::Strings::ERROR, Lang::Strings::PIN_ERROR);
                Ui::SetStatusText(Lang::Strings::ERROR);
                Ui::SetEmotion("triangle_exclamation");
                Ui::SetChatMessage("system", Lang::Strings::PIN_ERROR);
                PlayRespakAudio("err_pin");
                break;
            case NetworkEvent::ModemErrorRegDenied:
                ESP_LOGW(TAG, "[%s] %s: %s", "triangle_exclamation",
                         Lang::Strings::ERROR, Lang::Strings::REG_ERROR);
                Ui::SetStatusText(Lang::Strings::ERROR);
                Ui::SetEmotion("triangle_exclamation");
                Ui::SetChatMessage("system", Lang::Strings::REG_ERROR);
                PlayRespakAudio("err_reg");
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Ui::SetStatus(Ui::Status::DetectingModule);
                Ui::SetChatMessage("system", Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorTimeout:
                Ui::SetStatus(Ui::Status::RegisteringNetwork);
                break;
        }
        PublishWifiIcon();
    });

    // 异步启动网络。
    board.StartNetwork();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    // 立即更新状态栏以显示当前网络状态。
    Ui::UpdateStatusBar(true);

    // 初始化阶段必须等 OTA 检查与更新结束后，才能读取激活信息并创建小聆连接。
    // 等待期间仍复用应用事件分发器，保证升级确认按键、UI 调度和网络事件可用。
    constexpr UBaseType_t kApplicationUiPriority = 5;
    vTaskPrioritySet(nullptr, kApplicationUiPriority);
    ESP_LOGI(TAG, "应用/UI 调度优先级: %u",
             static_cast<unsigned>(kApplicationUiPriority));
    if (!WaitForOtaFlowCompletion()) {
        ESP_LOGE(TAG, "OTA 流程失败，停止设备激活和小聆连接");
        SetDeviceState(kDeviceStateIdle);
        const bool resource_restart_pending =
            ota_flow_.resource_restart_pending.load();
        Ui::SetStatusText(resource_restart_pending
                              ? Lang::Strings::RESOURCE_UPDATE_FAILED
                              : Lang::Strings::ERROR);
        Ui::SetEmotion("circle_xmark");
        Ui::SetChatMessage(
            "system", resource_restart_pending
                          ? Lang::Strings::RESOURCE_UPDATE_RESTARTING
                          : last_error_message_);
        if (!resource_restart_pending) {
            PlayTone(94);
        }
        return;
    }

    RunXiaolingActivation();
    InitializeXiaolingConn();

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);
    const std::string version = esp_app_get_description()->version;
    Ui::ShowNotification(std::string(Lang::Strings::VERSION) + version);
    Ui::SetChatMessage("system", "");
    PlayTone(59);
}

void Application::Run() {
    // 启动/OTA 阶段仍为低优先级。运行期需要及时执行唤醒/打断控制，
    // 不能让 priority=8 的持续录音编码把紧急队列压在 priority=5 后面。
    constexpr UBaseType_t kApplicationControlPriority = 9;
    vTaskPrioritySet(nullptr, kApplicationControlPriority);
    ESP_LOGI(TAG, "运行期语音控制优先级: %u",
             static_cast<unsigned>(kApplicationControlPriority));
    while (true) {
        const EventBits_t bits = xEventGroupWaitBits(
            event_group_, kAllMainEvents, pdTRUE, pdFALSE, portMAX_DELAY);
        ProcessMainEvents(bits);
    }
}

bool Application::WaitForOtaFlowCompletion() {
    ESP_LOGI(TAG, "等待 OTA 流程完成");
    while (!ota_flow_.completed.load()) {
        const EventBits_t bits = xEventGroupWaitBits(
            event_group_, kAllMainEvents, pdTRUE, pdFALSE, portMAX_DELAY);
        ProcessMainEvents(bits);
    }
    ESP_LOGI(TAG, "OTA 流程完成，结果: %s",
             ota_flow_.succeeded.load() ? "成功" : "失败");
    return ota_flow_.succeeded.load();
}

void Application::ProcessMainEvents(EventBits_t bits) {
    if (bits & MAIN_EVENT_ERROR) {
        SetDeviceState(kDeviceStateIdle);
        ESP_LOGW(TAG, "[circle_xmark] %s: %s",
                 Lang::Strings::ERROR, last_error_message_.c_str());
        Ui::SetStatusText(Lang::Strings::ERROR);
        Ui::SetEmotion("circle_xmark");
        Ui::SetChatMessage("system", last_error_message_);
        PlayTone(94);
    }

    if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
        HandleNetworkConnectedEvent();
    }

    if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
        HandleNetworkDisconnectedEvent();
    }

    if (bits & MAIN_EVENT_STATE_CHANGED) {
        HandleStateChangedEvent();
    }

    if (bits & MAIN_EVENT_TOGGLE_CHAT) {
        HandleToggleChatEvent();
    }

    if (bits & MAIN_EVENT_START_LISTENING) {
        HandleStartListeningEvent();
    }

    if (bits & MAIN_EVENT_STOP_LISTENING) {
        HandleStopListeningEvent();
    }

    if (bits & MAIN_EVENT_SCHEDULE) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto urgent_tasks = std::move(main_urgent_tasks_);
        auto tasks = std::move(main_tasks_);
        lock.unlock();
        for (auto& task : urgent_tasks) {
            task();
        }
        for (auto& task : tasks) {
            // 普通任务执行期间新到的唤醒也要抢在剩余普通任务之前。
            std::deque<std::function<void()>> incoming_urgent;
            {
                std::lock_guard<std::mutex> urgent_lock(mutex_);
                incoming_urgent.swap(main_urgent_tasks_);
            }
            for (auto& urgent : incoming_urgent) urgent();
            task();
        }
    }

    if (bits & MAIN_EVENT_CLOCK_TICK) {
        if (xiaoling_conn_) xiaoling_conn_->CheckVoiceStartTimeout();
        clock_ticks_++;
        if (alarm_flow_.on_clock_tick) {
            alarm_flow_.on_clock_tick();
        }
        PublishWifiIcon();
        Ui::UpdateStatusBar();

        if (GetDeviceState() == kDeviceStateListening && listening_started_at_us_ > 0) {
            const int32_t timeout_ms = KvUser::GetIdleExitTimeoutMs();
            const int64_t elapsed_ms =
                (esp_timer_get_time() - listening_started_at_us_) / 1000;
            if (timeout_ms > 0 && elapsed_ms >= timeout_ms) {
                ESP_LOGI(TAG, "录音超时(%ld ms)，退出录音回到待命状态",
                         static_cast<long>(timeout_ms));
                if (xiaoling_conn_) {
                    xiaoling_conn_->SendStopListening();
                }
                SetDeviceState(kDeviceStateIdle);
                listening_started_at_us_ = 0;
                // 当前就在 Application 主任务中，先让超时提示音占住栈顶，
                // 再退出语音会话。这样提示音结束后，Intent 管理器会恢复
                // 被唤醒流程暂停的 MusicIntent；若先退出语音会话，音乐会在
                // 提示音入栈前短暂恢复播放。
                PlayPromptTone(72);
                intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
            }
        }

        // 每 10 秒输出一次调试信息。
        if (clock_ticks_ % 10 == 0) {
            SystemInfo::PrintHeapStats();
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        StartOtaFlow();
    }

    // 立即更新状态栏以显示当前网络状态。
    Ui::UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    audio_player_.Stop(PlaybackEndReason::kCancelled);

    // 立即更新状态栏以显示当前网络状态。
    Ui::UpdateStatusBar(true);
}

void Application::RunXiaolingActivation() {
    SetDeviceState(kDeviceStateActivating);
    XiaolingConnActivation activation;
    while (true) {
        if (!activation.Fetch()) {
            ESP_LOGW(TAG, "未取得小聆激活信息，10 秒后重试");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        if (!activation.IsRequired()) {
            Ui::DismissQrCode();
            return;
        }
        if (!activation.code().empty()) {
            ShowActivationCode(activation.code(), activation.qrcode());
        }

        while (true) {
            ESP_LOGI(TAG, "等待设备绑定");
            const esp_err_t result = activation.Submit();
            if (result == ESP_OK) {
                ESP_LOGI(TAG, "设备绑定成功");
                Ui::DismissQrCode();
                return;
            }

            if (result == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }

            // 非“等待绑定”响应可能表示网络异常或 challenge 已失效。
            // 延迟后重新获取激活信息，必要时同步刷新二维码和 challenge。
            ESP_LOGW(TAG, "设备绑定查询失败，10 秒后重新获取激活信息");
            vTaskDelay(pdMS_TO_TICKS(10000));
            break;
        }
    }
}

void Application::ShowActivationCode(const std::string& code,
                                     const std::string& qrcode) {
    const std::string activation_text =
        std::string(Lang::Strings::ACTIVATION_INSTRUCTIONS) + code;

    ESP_LOGI(TAG, "显示激活码: %s, 二维码 URL: %s",
             code.c_str(), qrcode.c_str());
    if (!qrcode.empty()) {
        auto* fetcher = Board::GetInstance().GetImageFetcher();
        auto image = fetcher != nullptr ? fetcher->Fetch(qrcode) : nullptr;
        if (image) {
            Ui::ShowQrCode(std::move(image), activation_text);
        } else {
            ESP_LOGE(TAG, "激活二维码图片下载失败");
        }
    } else {
        ESP_LOGW(TAG, "激活二维码 URL 为空");
    }

    PlayActivationAudio("activation");
    for (const char digit : code) {
        if (digit >= '0' && digit <= '9') {
            const char name[] = {digit, '\0'};
            PlayActivationAudio(name);
        }
    }
}

void Application::InitializeXiaolingConn() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // 单工/半双工/全双工是小聆会话策略，不属于音频模块初始化。
    // 未保存模式时，具备 AEC 能力的板型默认全双工，否则默认半双工。
    const int32_t saved_mode = KvUser::GetIntMode();
    DuplexMode mode = audio_service_.SupportsAec()
                          ? DuplexMode::kFullDuplex
                          : DuplexMode::kHalfDuplex;
    if (saved_mode == 0) {
        mode = DuplexMode::kFullDuplex;
    } else if (saved_mode == 1) {
        mode = DuplexMode::kHalfDuplex;
    } else if (saved_mode == 2) {
        mode = DuplexMode::kSimplex;
    }
    if (mode == DuplexMode::kFullDuplex && !audio_service_.SupportsAec()) {
        ESP_LOGW(TAG, "当前板型不支持 AEC，全双工配置回退为半双工");
        mode = DuplexMode::kHalfDuplex;
    }
    session_controller_.SetMode(mode);
    listening_mode_ = mode == DuplexMode::kFullDuplex
                          ? kListeningModeRealtime
                          : kListeningModeAutoStop;
    audio_service_.SetAecEnabled(session_controller_.policy().enable_aec);

    Ui::SetStatus(Ui::Status::LoadingProtocol);

    // 小聆只使用 HTTP 鉴权和 WebSocket 会话。
    xiaoling_conn_ = std::make_unique<XiaolingConn>();

    xiaoling_conn_->OnConnected([this, codec, &board]() {
        SdMusicService::GetInstance().OnCloudReady();
        if (GetDeviceState() == kDeviceStateIdle) {
            Ui::SetStatus(Ui::Status::Standby);
            Ui::SetEmotion("neutral");
            Ui::SetChatMessage("system", "");
        }
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        Ui::SetDuplexMode(session_controller_.policy().mode);
        Ui::SetDuplexVisible(true);
        if (xiaoling_conn_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "服务端采样率 %d 与设备输出采样率 %d 不一致",
                     xiaoling_conn_->server_sample_rate(), codec->output_sample_rate());
        }
        // A transport reconnect resets the cloud voice session. If it happened
        // while waiting for speech, establish a fresh session instead of
        // leaving the microphone producer detached from the cloud forever.
        Schedule([this]() {
            if (GetDeviceState() != kDeviceStateListening ||
                xiaoling_conn_ == nullptr || xiaoling_conn_->CanUploadAudio()) {
                return;
            }
            ESP_LOGI(TAG, "WebSocket 重连完成，恢复全双工语音会话");
            xiaoling_conn_->SendStartListening(listening_mode_);
            RefreshListeningAudioUpload();
        });
    });

    xiaoling_conn_->OnNetworkError([this](XiaolingError error) {
        switch (error) {
            case XiaolingError::ServerNotConnected:
                last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
                break;
            case XiaolingError::ServerTimeout:
                last_error_message_ = Lang::Strings::SERVER_TIMEOUT;
                break;
            case XiaolingError::ServerError:
                last_error_message_ = Lang::Strings::SERVER_ERROR;
                break;
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    xiaoling_conn_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
#ifdef CONFIG_LSPLATFORM
        downlink_watchdog_.Feed(packet->frame_duration);
#endif  // 小聆平台
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

#ifdef CONFIG_LSPLATFORM
    xiaoling_conn_->OnAudioSent([this](uint32_t duration_ms) {
        uplink_watchdog_.Feed(duration_ms);
    });
#endif  // 小聆平台

    xiaoling_conn_->OnDisconnected([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        SdMusicService::GetInstance().OnCloudDisconnected();
        Ui::SetDuplexVisible(false);
        Schedule([this]() {
            if (GetDeviceState() == kDeviceStateWifiConfiguring) {
                return;
            }
            Ui::SetChatMessage("system", "");
            if (audio_player_.IsWakeupPending()) {
                CancelWakeup();
                return;
            }
            if (!audio_player_.IsPlaying()) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });

    xiaoling_conn_->OnIncomingJson([this](const cJSON* root, uint32_t generation) {
        const auto* action = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (!cJSON_IsString(action) || action->valuestring == nullptr) {
            ESP_LOGW(TAG, "lschat message has no valid action");
            return;
        }

        if (strcmp(action->valuestring, "mcp") == 0) {
            const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsObject(data)) {
                McpServer::GetInstance().ParseMessage(data);
            } else {
                ESP_LOGW(TAG, "Invalid lschat MCP message");
            }
            return;
        }

        if (strcmp(action->valuestring, "result") == 0) {
            // 普通 result 的 IAT/TTS 在语音层转换为带 epoch 的事件。
            const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            const auto* sub = cJSON_IsObject(data)
                                  ? cJSON_GetObjectItemCaseSensitive(data, "sub")
                                  : nullptr;
            if (!cJSON_IsString(sub) || sub->valuestring == nullptr) {
                ESP_LOGW(TAG, "Invalid lschat result message");
                return;
            }

            if (strcmp(sub->valuestring, "setting") == 0) {
                Ui::HandleSettingMessage(data);
            }
            return;
        }

        if (strcmp(action->valuestring, "started") == 0) {
            const auto request_id = xiaoling_conn_->session_id();
            ScheduleUrgent([this, generation, request_id]() {
                if (!Xiaoling::voice_session_epoch.IsCurrent(generation) ||
                    !xiaoling_conn_ || xiaoling_conn_->session_id() != request_id ||
                    !xiaoling_conn_->CanUploadAudio()) return;
                const auto state = GetDeviceState();
                if (state != kDeviceStateListening &&
                    !(state == kDeviceStateSpeaking && session_controller_.KeepListeningDuringTts())) return;
                RefreshListeningAudioUpload();
                ESP_LOGI(TAG, "新语音会话确认，开启音频上报: epoch=%lu",
                         static_cast<unsigned long>(generation));
            });
            return;
        }

        if (strcmp(action->valuestring, "finish") == 0 ||
            strcmp(action->valuestring, "closed") == 0) {
            const bool natural_finish = strcmp(action->valuestring, "finish") == 0;
            const auto request_id = xiaoling_conn_->session_id();
            Schedule([this, natural_finish, generation, request_id]() {
                if (!Xiaoling::voice_session_epoch.IsCurrent(generation) ||
                    !xiaoling_conn_ || xiaoling_conn_->session_id() != request_id) {
                    ESP_LOGI(TAG, "忽略排队的旧会话结束事件: epoch=%lu",
                             static_cast<unsigned long>(generation));
                    return;
                }
                const bool has_music = intent_manager_.HasUnpausedMusic();
                const bool tts_active = active_tts_session_id_ != 0 ||
                                        pending_tts_session_id_ != 0 ||
                                        tts_waiting_for_playback_.load();
                if (has_music) {
                    // TTS 仍在播放时保留 VoiceSessionIntent 压住音乐；
                    // TTS 结束回调会再根据栈中的 MusicIntent 完成退出。
                    if (tts_active) {
                        return;
                    }
                    iat_utterance_active_ = false;
                    audio_service_.EnableAudioUpload(false);
                    session_controller_.EndSession();
                    if (GetDeviceState() != kDeviceStateIdle) {
                        SetDeviceState(kDeviceStateIdle);
                    }
                    intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
                    return;
                }
                intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
                const auto state = GetDeviceState();
                if (natural_finish &&
                    session_controller_.policy().mode == DuplexMode::kFullDuplex &&
                    (state == kDeviceStateListening || state == kDeviceStateSpeaking) &&
                    xiaoling_conn_ && xiaoling_conn_->IsConnected() &&
                    !xiaoling_conn_->IsSessionActive()) {
                    ESP_LOGI(TAG, "全双工云端会话已结束，立即重启 AEC 音频上行");
                    xiaoling_conn_->SendStartListening(kListeningModeRealtime);
                } else if (state == kDeviceStateListening) {
                    SetDeviceState(kDeviceStateIdle);
                }
            });
        } else if (strcmp(action->valuestring, "error") == 0) {
            const auto* code = cJSON_GetObjectItemCaseSensitive(root, "code");
            ESP_LOGE(TAG, "lschat session error: %s",
                     cJSON_IsString(code) && code->valuestring != nullptr ? code->valuestring : "unknown");
        }
    });

    Ui::SetEmotion("microchip_ai");
    if (xiaoling_conn_->Start()) {
        ESP_LOGI(TAG, "小聆连接和语音模块已就绪");
    } else {
        ESP_LOGE(TAG, "小聆连接失败，等待连接任务自动重连");
    }
}

bool Application::EnsureXiaolingReadyForWakeup() {
    if (xiaoling_conn_->IsConnected()) {
        return true;
    }
    if (network_connected_) {
        // 网络正常但小聆长连接未就绪（连接中/重连中/会话异常）。
        ESP_LOGW(TAG, "小聆 WebSocket 尚未连接，播报连接失败提示音");
        PlayTone(85);
    } else {
        ESP_LOGW(TAG, "网络未连接，播报网络未连接提示音");
        PlayTone(64);
    }
    return false;
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    if (SdMusicService::GetInstance().IsBusy()) {
        ESP_LOGI(TAG, "TF 卡文件同步期间忽略语音交互按键");
        return;
    }
    if (audio_player_.IsWakeupPending()) {
        CancelWakeup();
        return;
    }
    auto state = GetDeviceState();

    if (state == kDeviceStateCheckingUpdate ||
        state == kDeviceStateActivating) {
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!xiaoling_conn_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!EnsureXiaolingReadyForWakeup()) {
            return;
        }

        BeginWakeup(listening_mode_);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        SetDeviceState(kDeviceStateIdle);
        if (EnsureXiaolingReadyForWakeup()) {
            BeginWakeup(listening_mode_);
        }
    } else if (state == kDeviceStateListening) {
        // 按键操作表示取消交互，而不是提交本次语音。先停止采集以立即给出
        // 本地反馈，再取消云端会话，防止其返回最终 IAT/TTS。
        ESP_LOGI(TAG, "按键取消聆听：立即停止本地录音并取消云端会话");
        iat_utterance_active_ = false;
        audio_service_.EnableAudioUpload(false);
        session_controller_.EndSession();
        SetDeviceState(kDeviceStateIdle);
        if (!xiaoling_conn_->CancelSession()) {
            ESP_LOGW(TAG, "按键取消云端语音会话上报失败");
        }
    }
}

void Application::HandleStartListeningEvent() {
    if (audio_player_.IsWakeupPending()) {
        return;
    }
    auto state = GetDeviceState();

    if (state == kDeviceStateCheckingUpdate ||
        state == kDeviceStateActivating) {
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!xiaoling_conn_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!EnsureXiaolingReadyForWakeup()) {
            return;
        }

        BeginWakeup(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        SetDeviceState(kDeviceStateIdle);
        if (EnsureXiaolingReadyForWakeup()) {
            BeginWakeup(kListeningModeManualStop);
        }
    }
}

void Application::HandleStopListeningEvent() {
    if (audio_player_.IsWakeupPending()) {
        CancelWakeup();
        return;
    }
    auto state = GetDeviceState();

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (xiaoling_conn_) {
            xiaoling_conn_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent(const std::string& wake_word) {
    if (audio_service_.IsLocalEncodingBenchmark()) return;
    if (wakeup_recording_gate_.Blocked()) {
        ESP_LOGI(TAG, "无 AEC 唤醒提示音期间忽略重复唤醒");
        return;
    }
    if (alarm_flow_.on_wake_word) {
        alarm_flow_.on_wake_word();
    }
    HandleWakeWordInvocation(wake_word);
}

void Application::PublishWifiIcon() {
    std::string icon;
    if (GetDeviceState() == kDeviceStateWifiConfiguring) {
        icon = FONT_AWESOME_WIFI_SLASH;
    } else if (const char* network_icon = Board::GetInstance().GetNetworkStateIcon();
               network_icon != nullptr) {
        icon = network_icon;
    }
    Ui::SetWifiIcon(std::move(icon));
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    if (reason == kAbortReasonExit) {
        exit_requested_.store(true);
        // exit MCP 工具只结束当前上行对话。TTS 可能已经开始下载或播放，
        // 因此这里不操作播放器。
        iat_utterance_active_ = false;
        audio_service_.EnableAudioUpload(false);
        session_controller_.EndSession();
        if (xiaoling_conn_) xiaoling_conn_->SendStopListening();
        const bool tts_active = tts_waiting_for_playback_.load() ||
                                active_tts_session_id_ != 0 ||
                                audio_player_.IsPlaying();
        if (tts_active) {
            audio_service_.EnableWakeWordDetection(true);
            ESP_LOGI(TAG, "退出请求：保留 TTS 播放和唤醒检测，允许开启新对话");
        } else {
            exit_requested_.store(false);
        }
        SetDeviceState(kDeviceStateIdle);
        ESP_LOGI(TAG, "退出请求：已停止上行，保留播放器继续播放");
        return;
    }
    aborted_ = true;

    // 半双工模式下，exit 工具到达时麦克风会话可能仍在监听。取消讲话只会
    // 停止播放，不会关闭云端会话，因此这里需要显式发送会话结束消息。
    if (GetDeviceState() == kDeviceStateListening) {
        iat_utterance_active_ = false;
        audio_service_.EnableAudioUpload(false);
        session_controller_.EndSession();
        if (xiaoling_conn_ && !xiaoling_conn_->EndSession()) {
            ESP_LOGW(TAG, "Failed to end the listening session while aborting");
        }
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (xiaoling_conn_) {
        // 必须在播放器 STOPPED 回调清理 timeline 之前冻结当前 PCM 时钟游标。
        xiaoling_conn_->PrepareAbortSpeaking(reason);
    }
    if (reason == kAbortReasonWakeWordDetected) {
        audio_player_.StopNonMusic(PlaybackEndReason::kPreempted);
    } else {
        audio_player_.Stop(PlaybackEndReason::kCancelled);
    }
    if (xiaoling_conn_) {
        xiaoling_conn_->SendAbortSpeaking(reason);
    }
}

void Application::BeginWakeup(ListeningMode mode, const std::string& wake_word) {
    bool interrupted_tts = false;
    if (!xiaoling_conn_ || !ResetVoiceSessionForWakeup(interrupted_tts)) return;
    // 重开会话继续持有 VoiceSessionIntent，避免两轮对话之间恢复被暂停的音乐。
    intent_manager_.Push(PlaybackIntentId::kVoiceSession);
    exit_requested_.store(false);
    if (!SetDeviceState(kDeviceStateConnecting)) {
        intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
        return;
    }
    iat_utterance_active_ = false;
    audio_service_.EnableAudioUpload(false);
    if (!intent_manager_.Push(PlaybackIntentId::kPromptTone)) {
        ESP_LOGE(TAG, "唤醒提示音事件入栈失败");
        SetDeviceState(kDeviceStateIdle);
        intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
        return;
    }
    const auto generation = Xiaoling::voice_session_epoch.Current();
    wakeup_recording_gate_.Begin(generation, audio_service_.IsAecEnabled());
    if (wakeup_recording_gate_.Blocked()) {
        audio_service_.SetMicrophoneSuppressed(true);
        audio_service_.PauseWakeWordDetection();
        ESP_LOGI(TAG, "无 AEC：按提示音实际播放终态丢弃录音，不限制 tone 长度");
    }
    audio_player_.PlayWakeup([this, generation](bool proceed) {
        Schedule([this, generation, proceed]() {
            if (!Xiaoling::voice_session_epoch.IsCurrent(generation)) return;
            const bool deferred_recording = wakeup_recording_gate_.Complete(generation);
            if (deferred_recording) audio_service_.SetMicrophoneSuppressed(false);
            intent_manager_.Pop(PlaybackIntentId::kPromptTone);
            if (!proceed) {
                if (GetDeviceState() == kDeviceStateListening && xiaoling_conn_) {
                    xiaoling_conn_->SendStopListening();
                }
                if (xiaoling_conn_) xiaoling_conn_->SendWakeWordDetected("");
                SetDeviceState(kDeviceStateIdle);
                intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
                return;
            }
            if (deferred_recording && GetDeviceState() == kDeviceStateConnecting) {
                audio_service_.EnableWakeWordDetection(true);
                ESP_LOGI(TAG, "无 AEC：提示音及 DMA 已结束，开始新录音会话");
                SetListeningMode(listening_mode_);
                return;
            }
            ESP_LOGI(TAG, "Wakeup tone finished; recording session remains active");
        });
    });
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }
    // tone 已交给播放器，在本地开始解码/出声；旧会话的网络控制在其后执行。
    // 保持 event/cancel/start 的协议顺序，但不让网络发送挡住本地提示音。
    if (interrupted_tts) {
        xiaoling_conn_->SendAbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (!xiaoling_conn_->EndSession()) {
        ESP_LOGE(TAG, "Failed to report the current session end before wakeup");
        CancelWakeup();
        return;
    }
    if (!wake_word.empty()) xiaoling_conn_->SendWakeWordDetected(wake_word);
    // AEC 开启时继续并行录音；否则等本轮 tone 自然结束并排空 DMA 再建会话，
    // 让自定义长 tone 不占用录音和云端会话的超时预算。ADC/处理器仍常驻。
    if (!wakeup_recording_gate_.Blocked()) {
        ESP_LOGI(TAG, "Wakeup tone submitted; start new recording session in parallel");
    }
    SetListeningMode(mode);
    if (iat_subscription_id_ == 0) {
        auto on_iat_text = [this](const AppEvent::Event& event) {
            // 事件已经在 app_event 任务中串行执行；仍记录接收时的 TTS 世代，
            // 防止排队中的旧 IAT 打断后续 TTS。
            const uint32_t observed_tts_generation = tts_url_generation_.load();
            const uint32_t received_ms = esp_log_timestamp();
            const auto scope = event.Scope();
            ScheduleUrgent([this, text = std::string(event.Text()), observed_tts_generation,
                            received_ms, scope]() {
                if (!scope.IsCurrent()) return;
                if (text.empty()) {
                    return;
                }
                const bool first_iat = !iat_utterance_active_;
                if (first_iat) {
                    iat_utterance_active_ = true;
                }
                if (first_iat && GetDeviceState() == kDeviceStateListening) {
                    // 有效的 IAT 结果说明用户已经说话。重新开始空闲计时窗口，
                    // 让超时统计从用户说完后的静音开始，而不是从唤醒时开始。
                    listening_started_at_us_ = esp_timer_get_time();
                    ESP_LOGI(TAG, "收到 IAT，重置录音空闲计时");
                }
                const bool tts_active = active_tts_session_id_ != 0 ||
                                        tts_waiting_for_playback_.load();
                if (session_controller_.policy().allow_barge_in &&
                    !audio_player_.IsWakeupPending() && tts_active) {
                    const uint32_t current_tts_generation = tts_url_generation_.load();
                    if (observed_tts_generation != current_tts_generation) {
                        if (first_iat) {
                            ESP_LOGI(TAG,
                                     "跳过延迟 IAT 对新 TTS 的误打断: observed=%lu, "
                                     "current=%lu, delay=%lums, bytes=%u",
                                     static_cast<unsigned long>(observed_tts_generation),
                                     static_cast<unsigned long>(current_tts_generation),
                                     static_cast<unsigned long>(esp_log_timestamp() - received_ms),
                                     static_cast<unsigned>(text.size()));
                        }
                        return;
                    }
                    if (interrupted_tts_generation_ == current_tts_generation) {
                        return;
                    }
                    interrupted_tts_generation_ = current_tts_generation;
                    // 全双工模式在播放 TTS 时会刻意保持监听状态，因此不能用
                    // DeviceStatus 判断用户是否打断。非空 IAT 结果才是用户已开始
                    // 新一轮讲话的可靠依据。
                    ESP_LOGI(TAG,
                             "非空 IAT 打断 TTS: session=%lu, generation=%lu, "
                             "waiting=%d, delay=%lums, bytes=%u",
                             static_cast<unsigned long>(active_tts_session_id_),
                             static_cast<unsigned long>(current_tts_generation),
                             tts_waiting_for_playback_.load() ? 1 : 0,
                             static_cast<unsigned long>(esp_log_timestamp() - received_ms),
                             static_cast<unsigned>(text.size()));
                    if (xiaoling_conn_) {
                        xiaoling_conn_->HandleIatBargeIn();
                    }
                    if (!audio_player_.StopNonMusic(PlaybackEndReason::kPreempted)) {
                        ESP_LOGW(TAG, "非空 IAT 停止本地 TTS 失败");
                    }
                }
            });
        };
        iat_subscription_id_ = AppEvent::Subscribe(
            AppEvent::Id::XiaolingIatStart, on_iat_text);
        iat_update_subscription_id_ = AppEvent::Subscribe(
            AppEvent::Id::XiaolingIatUpdate, on_iat_text);
        iat_end_subscription_id_ = AppEvent::Subscribe(
            AppEvent::Id::XiaolingIatEnd, [this](const AppEvent::Event& event) {
                const auto scope = event.Scope();
                Schedule([this, text = std::string(event.Text()), scope]() {
                    if (!scope.IsCurrent()) return;
                    iat_utterance_active_ = false;
                    // 非空的最终 IAT 表示用户已经说完。在 TTS URL 到达前显示云端
                    // 思考状态。URL 回调会先设置此标志，再将自己的 UI 更新入队，
                    // 避免延迟的 IAT 任务把界面从讲话状态闪回思考状态。
                    if (!text.empty() && !tts_waiting_for_playback_.load() &&
                        GetDeviceState() == kDeviceStateListening) {
                        Ui::SetStatusText("思考中...");
                        Ui::SetEmotion("等待");
                    }
                });
            });
    }
}

bool Application::ResetVoiceSessionForWakeup(bool& interrupted_tts) {
    wakeup_recording_gate_.Cancel();
    audio_service_.SetMicrophoneSuppressed(false);
    const auto state = GetDeviceState();
    const auto generation = Xiaoling::voice_session_epoch.Advance();
    AppEvent::PublishScoped(AppEvent::Id::VoiceSessionStarted, nullptr, 0,
        Xiaoling::voice_session_epoch, generation);
    const bool tts_active = active_tts_session_id_ != 0 || pending_tts_session_id_ != 0 ||
                            tts_waiting_for_playback_.load();
    interrupted_tts = tts_active;
    ESP_LOGI(TAG, "唤醒替换语音会话: state=%s, epoch=%lu, old_tts=%lu/%lu",
             DeviceStatus::GetStateName(state), static_cast<unsigned long>(generation),
             static_cast<unsigned long>(active_tts_session_id_),
             static_cast<unsigned long>(pending_tts_session_id_));
    iat_utterance_active_ = false;
    audio_service_.EnableAudioUpload(false);
#ifdef CONFIG_LSPLATFORM
    uplink_watchdog_.Stop();
    downlink_watchdog_.Stop();
#endif
    exit_requested_.store(false);
    tts_url_generation_.fetch_add(1);
    interrupted_tts_generation_ = 0;
    // 先使旧播放标识失效，再停播放器；迟到终态不能恢复旧录音或弹出新 Intent。
    active_tts_session_id_ = 0;
    pending_tts_session_id_ = 0;
    tts_waiting_for_playback_.store(false);
    tts_output_started_.store(false);
    session_controller_.EndSession();
    // 先冻结旧回复游标，再立即停止本地声音；网络 cancel/event 不得拖住静音。
    if (xiaoling_conn_ && tts_active) {
        xiaoling_conn_->PrepareAbortSpeaking(kAbortReasonWakeWordDetected);
    }
    audio_player_.CancelWakeup();
    audio_player_.StopNonMusic(PlaybackEndReason::kPreempted);
    prompt_tone_session_id_.store(0);
    // 唤醒提示音的旧完成回调已经因 epoch 变化失效，主动释放它的 Intent。
    intent_manager_.Pop(PlaybackIntentId::kPromptTone);
    SetDeviceState(kDeviceStateIdle);
    return xiaoling_conn_ != nullptr;
}

void Application::PauseWakeWordDetection() {
    audio_service_.PauseWakeWordDetection();
    audio_service_.EnableWakeWordDetection(true);
}

void Application::CancelWakeup() {
    wakeup_recording_gate_.Cancel();
    audio_service_.SetMicrophoneSuppressed(false);
    audio_player_.CancelWakeup();
    intent_manager_.Pop(PlaybackIntentId::kPromptTone);
    if (xiaoling_conn_) {
        if (GetDeviceState() == kDeviceStateListening) {
            xiaoling_conn_->SendStopListening();
        }
        xiaoling_conn_->SendWakeWordDetected("");
    }
    SetDeviceState(kDeviceStateIdle);
    intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    if (wakeup_recording_gate_.Blocked()) return;
    SetDeviceState(kDeviceStateListening);
}

void Application::RefreshListeningAudioUpload() {
    const auto state = GetDeviceState();
    const bool enable = !wakeup_recording_gate_.Blocked() && xiaoling_conn_ &&
                        xiaoling_conn_->CanUploadAudio() &&
                        (state == kDeviceStateListening ||
                         (state == kDeviceStateSpeaking && session_controller_.KeepListeningDuringTts()));
    audio_service_.EnableAudioUpload(enable);
#ifdef CONFIG_LSPLATFORM
    if (enable) uplink_watchdog_.Start();
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = DeviceStatus::GetState();
    // AEC 并行监听，无 AEC 在 connecting 等 tone。在进入 listening 时保留
    // 该提示音，其他无关状态变化仍会将其取消并撤销录音保护。
    if (new_state != kDeviceStateConnecting &&
        new_state != kDeviceStateListening &&
        audio_player_.IsWakeupPending()) {
        wakeup_recording_gate_.Cancel();
        audio_service_.SetMicrophoneSuppressed(false);
        audio_player_.CancelWakeup();
        intent_manager_.Pop(PlaybackIntentId::kPromptTone);
        if (xiaoling_conn_) {
            xiaoling_conn_->SendWakeWordDetected("");
        }
    }
    if (new_state == kDeviceStateWifiConfiguring &&
        intent_manager_.Contains(PlaybackIntentId::kMusic)) {
        // 配网是独立的终止型业务。先停止播放器并退出音乐 Intent，随后播放
        // 070 提示音时栈内不再保留音乐，提示音结束后也不会恢复原曲。
        audio_player_.Stop(PlaybackEndReason::kCancelled);
        intent_manager_.Music().Exit();
        ESP_LOGI(TAG, "进入配网模式，已停止音乐并退出音乐事件");
    }
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            listening_started_at_us_ = 0;
            session_controller_.EndSession();
            if (tts_waiting_for_playback_.load() && !exit_requested_.load()) {
                Ui::SetStatusText("思考中...");
                Ui::SetEmotion("等待");
            } else {
                Ui::SetStatus(Ui::Status::Standby);
                Ui::SetEmotion("待机");
            }
#ifdef CONFIG_LSPLATFORM
            Ui::DismissOverlays();
#endif  // 小聆平台
            audio_service_.EnableAudioUpload(false);
            // 上报关闭不等于关闭独立唤醒；包括退出会话后仍在播放的 TTS。
            audio_service_.EnableWakeWordDetection(true);
#ifdef CONFIG_LSPLATFORM
            uplink_watchdog_.Stop();
            downlink_watchdog_.Stop();
#endif  // 小聆平台
            break;
        case kDeviceStateConnecting:
            // 唤醒提示音阶段的过渡态：连接已就绪，直接预告聆听，避免
            // "连接中..." 一闪而过。
            Ui::SetStatus(Ui::Status::Listening);
            Ui::SetEmotion("聆听");
            Ui::SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            listening_started_at_us_ = esp_timer_get_time();
            session_controller_.StartListening(listening_mode_ == kListeningModeRealtime);
            Ui::SetStatus(Ui::Status::Listening);
            Ui::SetEmotion("聆听");
            Ui::SetVoicePrompt(Lang::Strings::PLEASE_SPEAK);
#ifdef CONFIG_LSPLATFORM
            Ui::DismissOverlays();
#endif  // 小聆平台

            // 全双工模式下，云端会话与本地 AFE 的生命周期相互独立。服务端
            // finish 可以关闭前者，而 AEC 链路会按设计继续运行。
            if (xiaoling_conn_ && !xiaoling_conn_->CanUploadAudio()) {
                xiaoling_conn_->SendStartListening(listening_mode_);
            }

            // 采集、AFE/AEC 常驻；异步 started 确认后才开放新会话音频上报。
            RefreshListeningAudioUpload();

            break;
        case kDeviceStateSpeaking:
            Ui::SetStatus(Ui::Status::Speaking);
            Ui::SetEmotion("说话");

            // 音乐位于语音事件下层时停止录音；其他情况下仅在实时会话有效时
            // 保持 AEC 后的上行链路运行。
            if (intent_manager_.HasUnpausedMusic() ||
                !session_controller_.KeepListeningDuringTts()) {
                audio_service_.EnableAudioUpload(false);
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Stop();
#endif  // 小聆平台
            }
            // GMF 解码结束后仍需排空 TTS PCM；此时 IsPlaying() 已可能为 false。
            if (active_tts_session_id_ == 0 && !tts_waiting_for_playback_.load() &&
                !audio_player_.IsPlaying()) {
                audio_player_.ResetPlayback();
            } else if (active_tts_session_id_ != 0 && !audio_player_.IsPlaying()) {
                ESP_LOGI(TAG, "TTS 会话仍在排空，保留播放器 PCM 队列: session=%lu",
                         static_cast<unsigned long>(active_tts_session_id_));
            }
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableAudioUpload(false);
            break;
        default:
            // 无需处理。
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::ScheduleUrgent(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 紧急任务保持先进先出顺序，用于让 IAT 打断等实时语音事件
        // 先于可能阻塞的普通应用任务执行。
        main_urgent_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}
void Application::WakeWordInvoke(const std::string& wake_word) {
    const auto detected_ms = esp_log_timestamp();
    ScheduleUrgent([this, wake_word, detected_ms]() {
        ESP_LOGI(TAG, "唤醒控制开始: queue_delay=%lums",
                 static_cast<unsigned long>(esp_log_timestamp() - detected_ms));
        HandleWakeWordDetectedEvent(wake_word);
    });
}

void Application::HandleWakeWordInvocation(const std::string& wake_word) {
    if (audio_service_.IsLocalEncodingBenchmark()) return;
    if (CameraService::GetInstance().CancelVoiceMcpCountdown(
            "用户通过语音唤醒取消拍照")) {
        return;
    }
    if (CameraService::GetInstance().IsActive()) {
        ESP_LOGI(TAG, "拍照流程期间忽略外部唤醒词");
        return;
    }
    if (SdMusicService::GetInstance().IsBusy()) {
        ESP_LOGI(TAG, "TF 卡文件同步期间忽略外部唤醒词：%s", wake_word.c_str());
        return;
    }
    PauseWakeWordDetection();
    if (!xiaoling_conn_) {
        return;
    }

    auto state = GetDeviceState();

    if (state != kDeviceStateIdle && state != kDeviceStateListening &&
        state != kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Ignoring wake word while device is in %s state",
                 DeviceStatus::GetStateName(state));
        return;
    }
    if (!EnsureXiaolingReadyForWakeup()) return;
    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
    BeginWakeup(listening_mode_, wake_word);
}

void Application::SetAudioStressMode(bool enable) {
    Schedule([this, enable]() {
        if (enable && (GetDeviceState() != kDeviceStateIdle &&
                       GetDeviceState() != kDeviceStateWifiConfiguring)) {
            ESP_LOGW(TAG, "本地压力测试需在待命/配网状态开启");
            return;
        }
        if (enable && xiaoling_conn_ && xiaoling_conn_->IsSessionActive()) {
            ESP_LOGW(TAG, "云端会话仍活动，拒绝开启本地压力测试");
            return;
        }
        if (!enable && audio_service_.IsLocalEncodingBenchmark()) {
            audio_player_.StopNonMusic(PlaybackEndReason::kCancelled);
        }
        if (!audio_service_.SetLocalEncodingBenchmark(enable)) {
            ESP_LOGW(TAG, "本地压力测试切换失败，音频服务停止或上报已开启");
        }
    });
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (xiaoling_conn_ && xiaoling_conn_->IsConnected()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // 此时可以安全进入休眠模式。
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // 始终调度到主任务执行，以保证线程安全。
    Schedule([this, payload = std::move(payload)]() {
        if (xiaoling_conn_) {
            xiaoling_conn_->SendMcpMessage(payload);
        }
    });
}

void Application::SetDuplexMode(DuplexMode mode) {
    Schedule([this, mode]() {
        if (mode == DuplexMode::kFullDuplex && !audio_service_.SupportsAec()) {
            ESP_LOGW(TAG, "当前硬件不支持双工模式 %d，要求设备 AEC 和参考通道",
                     static_cast<int>(mode));
            return;
        }
        if (mode == session_controller_.policy().mode) {
            return;
        }

        const auto state = GetDeviceState();
        // 结束当前云端会话并只停止上行链路。保持处理器运行，可以在恢复唤醒词
        // 检测时避免拆除并重建 AFE。
        iat_utterance_active_ = false;
        audio_service_.EnableAudioUpload(false);
        session_controller_.EndSession();
        if (xiaoling_conn_ && !xiaoling_conn_->EndSession()) {
            ESP_LOGW(TAG, "切换交互模式时上报会话结束失败");
        }

        session_controller_.SetMode(mode);
        const bool enable_aec = session_controller_.policy().enable_aec;
        Ui::SetDuplexMode(mode);
        listening_mode_ = mode == DuplexMode::kFullDuplex
                              ? kListeningModeRealtime
                              : kListeningModeAutoStop;
        audio_service_.SetAecEnabled(enable_aec);
        Ui::ShowNotification(enable_aec ? Lang::Strings::RTC_MODE_ON
                                        : Lang::Strings::RTC_MODE_OFF);

        if (state == kDeviceStateConnecting ||
            state == kDeviceStateListening ||
            state == kDeviceStateSpeaking) {
            SetDeviceState(kDeviceStateIdle);
        }
        const char* name = mode == DuplexMode::kSimplex ? "simplex" :
                           mode == DuplexMode::kHalfDuplex ? "half-duplex" : "full-duplex";
        ESP_LOGI(TAG, "交互模式切换为 %s", name);
    });
}

void Application::PlayRespakAudio(const char* name) {
    if (name == nullptr) {
        ESP_LOGW(TAG, "本地化提示音不存在: %s", name != nullptr ? name : "(null)");
        return;
    }
    ResourceData resource;
    std::string path = std::string("/locales/") + Lang::CODE + "/" + name + ".ogg";
    if (!Resource::Get(ResourcePackage::Respak, path, resource)) {
        path = std::string("/locales/en-US/") + name + ".ogg";
        if (!Resource::Get(ResourcePackage::Respak, path, resource)) {
            ESP_LOGW(TAG, "本地化提示音不存在: %s", name);
            return;
        }
    }
    audio_player_.PlaySound(std::string_view(
        reinterpret_cast<const char*>(resource.address), resource.size));
}

void Application::PlayActivationAudio(const char* name) {
    if (name == nullptr) {
        return;
    }
    const bool is_digit = name[0] >= '0' && name[0] <= '9' && name[1] == '\0';
    const unsigned id = is_digit ? 301 + name[0] - '0' : 300;
    ResourceData resource;
    char path[96] = {};
    std::snprintf(path, sizeof(path), "/activation/%s/%03u_%s.ogg",
                  Lang::CODE, id, name);
    if (!Resource::Get(ResourcePackage::Respak, path, resource)) {
        std::snprintf(path, sizeof(path), "/activation/en-US/%03u_%s.ogg",
                      id, name);
        if (!Resource::Get(ResourcePackage::Respak, path, resource)) {
            ESP_LOGW(TAG, "激活音频不存在: %s", name);
            return;
        }
    }
    audio_player_.PlaySound(std::string_view(
        reinterpret_cast<const char*>(resource.address), resource.size));
}

void Application::PlayTone(uint16_t tone_id) {
    // 调用者可能位于网络、UI 或定时器任务，统一回到 Application 主任务
    // 操作 Intent 栈和播放器。
    Schedule([this, tone_id]() { PlayPromptTone(tone_id); });
}

void Application::PlayPromptTone(uint16_t tone_id) {
    const bool intent_already_present =
        intent_manager_.Contains(PlaybackIntentId::kPromptTone);
    if (!intent_manager_.Push(PlaybackIntentId::kPromptTone)) {
        ESP_LOGW(TAG, "提示音事件入栈失败: id=%u",
                 static_cast<unsigned>(tone_id));
        return;
    }

    const uint32_t session_id = audio_player_.PlayTone(tone_id);
    if (session_id == 0) {
        ESP_LOGW(TAG, "ROMFS tone 播放失败: id=%u", static_cast<unsigned>(tone_id));
        // 已存在的提示音事件可能属于唤醒流程或上一段提示音，不能误弹出。
        if (!intent_already_present && prompt_tone_session_id_.load() == 0) {
            intent_manager_.Pop(PlaybackIntentId::kPromptTone);
        }
        return;
    }
    prompt_tone_session_id_.store(session_id);
    ESP_LOGI(TAG, "通用提示音取得最高播放优先级: id=%u, session=%lu",
             static_cast<unsigned>(tone_id),
             static_cast<unsigned long>(session_id));
}

void Application::ResetXiaolingConn() {
    Schedule([this]() {
        std::lock_guard<std::mutex> lock(xiaoling_conn_mutex_);
        Xiaoling::voice_session_epoch.Advance();
        // 如果音频通道已打开，则将其关闭。
        if (xiaoling_conn_ && xiaoling_conn_->IsConnected()) {
            xiaoling_conn_->Disconnect();
        }
        // 重置协议对象。
        xiaoling_conn_.reset();
    });
}
