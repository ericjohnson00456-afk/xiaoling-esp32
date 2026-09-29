#include "application.h"
#include "board.h"
#include "display/display.h"
#include "system/system_info.h"
#include "audio_codec.h"
#include "xiaoling/conn/xiaoling_conn.h"
#include "xiaoling/conn/xiaoling_conn_activation.h"
#include "ui/i18n/lang_config.h"
#include "mcp_server.h"
#include "resource/resource.h"
#include "settings.h"
#include "serial_shell.h"
#include "ui/ui.h"
#include "xiaoling/xiaoling_events.h"
#include "shell/cmd/kv/kv_user.h"

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

#ifdef CONFIG_LSPLATFORM
static const int kBadNetworkSampleWindowSec = 60;
static const int kBadNetworkSampleCount = 3;
#endif

static constexpr EventBits_t kAllMainEvents =
    MAIN_EVENT_SCHEDULE |
    MAIN_EVENT_WAKE_WORD_DETECTED |
    MAIN_EVENT_VAD_CHANGE |
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

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif
    const int32_t saved_int_mode = KvUser::GetIntMode();
    const DuplexMode initial_mode = saved_int_mode == 0
                                        ? DuplexMode::kFullDuplex
                                        : saved_int_mode == 1
                                            ? DuplexMode::kHalfDuplex
                                            : saved_int_mode == 2
                                                ? DuplexMode::kSimplex
                                                : (aec_mode_ == kAecOnDeviceSide
                                                       ? DuplexMode::kFullDuplex
                                                       : DuplexMode::kHalfDuplex);
    session_controller_.SetMode(initial_mode);
    listening_mode_ = initial_mode == DuplexMode::kFullDuplex
                          ? kListeningModeRealtime
                          : kListeningModeAutoStop;

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
    if (iat_subscription_id_ != 0) {
        XiaolingEventBus::GetInstance().Unsubscribe(iat_subscription_id_);
    }
    if (iat_update_subscription_id_ != 0) {
        XiaolingEventBus::GetInstance().Unsubscribe(iat_update_subscription_id_);
    }
    if (iat_end_subscription_id_ != 0) {
        XiaolingEventBus::GetInstance().Unsubscribe(iat_end_subscription_id_);
    }
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::Initialize() {
    // 先挂载 ROMFS 格式资源。
    if (!Resource::Initialize()) {
        ESP_LOGE(TAG, "One or more required ROMFS resources are unavailable");
    }

    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // UI 模块自行创建任务并串行处理界面更新，Application 只发送状态。
    if (!Ui::Initialize()) {
        ESP_LOGE(TAG, "Home 页面初始化失败");
    }
    // 网络和激活检查完成前显示配网齿轮；加载协议时会在建立 WebSocket 前
    // 切换为 AI 图标。
    Ui::SetEmotion("gear");
    PublishWifiIcon();
    Ui::SetDuplexMode(session_controller_.policy().mode);
    // 显示开发板名称和版本信息。
    Ui::SetChatMessage("system", SystemInfo::GetUserAgent());

    // 初始化音频服务。
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    Ui::SetWakeWord(audio_service_.GetWakeWordName());
    audio_service_.EnableDeviceAec(aec_mode_ == kAecOnDeviceSide);
    audio_service_.Start();

    if (!audio_player_.Initialize(&audio_service_)) {
        ESP_LOGE(TAG, "TTS 播放器初始化失败");
    }
    intent_manager_.Initialize(*this);
    audio_player_.SetEventCallback([this](const PlaybackEvent& event) {
        Schedule([this, event]() {
            audio_player_.DispatchPlaybackEvent(event);
            if (event.channel != PlaybackChannel::kTts) {
                return;
            }
            if (event.state == PlaybackState::kStreamReady) {
                pending_tts_session_id_ = event.session_id;
                XiaolingEventBus::GetInstance().Publish(XIAOLING_TTS_STREAM_READY);
                return;
            }
            if (event.state == PlaybackState::kPlaying) {
                pending_tts_session_id_ = 0;
                active_tts_session_id_ = event.session_id;
                XiaolingEventBus::GetInstance().Publish(XIAOLING_TTS_PLAYING);
                session_controller_.StartTts(event.session_id);
                // 与收到 URL 的事件不同，kPlaying 表示 HTTP 响应中已经产生
                // 可以解码的 TTS 音频数据。
                Ui::SetStatus(Ui::Status::Speaking);
                Ui::SetEmotion("说话");
                if (!exit_requested_.load() &&
                    tts_output_started_.exchange(false) &&
                    (GetDeviceState() == kDeviceStateIdle ||
                     GetDeviceState() == kDeviceStateListening)) {
                    tts_waiting_for_playback_.store(false);
                    SetDeviceState(kDeviceStateSpeaking);
                }
                return;
            }
            if (event.state != PlaybackState::kCompleted &&
                event.state != PlaybackState::kStopped &&
                event.state != PlaybackState::kFailed) {
                return;
            }
            tts_waiting_for_playback_.store(false);
            tts_output_started_.store(false);
            const bool active_session = active_tts_session_id_ == event.session_id;
            const bool pending_session = pending_tts_session_id_ == event.session_id;
            if (!active_session && !pending_session) {
                return;
            }
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
            XiaolingEventBus::GetInstance().Publish(
                XIAOLING_TTS_STOPPED, reason_text);
            pending_tts_session_id_ = 0;
            active_tts_session_id_ = 0;
            if (active_session) {
                session_controller_.EndTts(event.session_id);
            }
            if (exit_requested_.load()) {
                // Exit 会结束本轮对话生命周期，但允许当前 TTS 流播放完毕，
                // 随后才恢复唤醒检测。
                ESP_LOGI(TAG, "退出请求：TTS 播放结束，恢复唤醒检测但保持待命");
                if (GetDeviceState() != kDeviceStateIdle) {
                    SetDeviceState(kDeviceStateIdle);
                } else {
                    // 延迟的 TTS 流开始前，退出处理已经把设备切换为空闲状态。
                    // kPlaying 只直接更新了 UI，没有修改 DeviceStatus，因此再次
                    // 切换为空闲状态不会触发任何操作，讲话表情会继续显示；这里
                    // 需要显式恢复空闲界面。
                    Ui::SetStatus(Ui::Status::Standby);
                    Ui::SetEmotion("待机");
                }
                audio_service_.EnableWakeWordDetection(false);
                xEventGroupClearBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
                audio_service_.EnableWakeWordDetection(true);
                exit_requested_.store(false);
                return;
            }
            if (intent_manager_.Contains(PlaybackIntentId::kMusic)) {
                // 音乐仍在栈中时，TTS 结束就是退出本轮录音并恢复音乐的屏障。
                ESP_LOGI(TAG, "TTS 播放结束，退出语音会话并恢复音乐");
                iat_utterance_active_ = false;
                audio_service_.EnableVoiceProcessing(false);
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
                           event.channel == PlaybackChannel::kTts &&
                           event.state == PlaybackState::kCompleted &&
                           session_controller_.policy().mode == DuplexMode::kHalfDuplex) {
                    ESP_LOGI(TAG, "半双工 TTS 播放完成，重新开始上行");
                    SetDeviceState(kDeviceStateListening);
                } else {
                    SetDeviceState(kDeviceStateIdle);
                }
            }
        });
    });

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        // 此回调在 Opus 编码完成后执行，不持有 AudioService 队列锁。
        // XiaolingConn::SendAudio 只会把数据包移入有界队列并唤醒网络任务，
        // 不会直接写入套接字。该交接不能放到低优先级的应用/UI 任务中：
        // TTS 期间 UI 和字幕工作可能让该任务延迟数秒，过去曾因此填满
        // AudioService 的 2.4 秒发送队列。
        auto* connection = xiaoling_conn_.get();
        if (connection == nullptr) {
            return;
        }
        if (!connection->IsSessionActive() &&
            audio_service_.GetUplinkDiagnostics().upload_enabled) {
            // A failed WebSocket resets the cloud session immediately. Stop
            // producing packets until the protocol reconnect callback creates
            // a new session; otherwise the codec task keeps feeding a dead
            // connection and floods the network worker with rejected packets.
            ESP_LOGW(TAG, "云端语音会话已失效，暂停本地音频上行");
            audio_service_.EnableAudioUpload(false);
            return;
        }
        constexpr size_t kMaxDirectHandoffPackets = 8;
        for (size_t i = 0; i < kMaxDirectHandoffPackets; ++i) {
            auto packet = audio_service_.PopPacketFromSendQueue();
            if (packet == nullptr) {
                break;
            }
            connection->SendAudio(std::move(packet));
        }
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_player_output_started = [this]() {
        if (!tts_waiting_for_playback_.load()) {
            return;
        }
        tts_output_started_.store(true);
        XiaolingEventBus::GetInstance().Publish(XIAOLING_TTS_OUTPUT_STARTED);
        Schedule([this]() {
            if (!exit_requested_.load() && active_tts_session_id_ != 0 &&
                tts_output_started_.exchange(false) &&
                (GetDeviceState() == kDeviceStateIdle ||
                 GetDeviceState() == kDeviceStateListening)) {
                tts_waiting_for_playback_.store(false);
                SetDeviceState(kDeviceStateSpeaking);
            }
        });
    };
    callbacks.on_player_output_progress = [](uint32_t audio_clock_ms) {
        XiaolingEventBus::GetInstance().Publish(
            XIAOLING_TTS_OUTPUT_PROGRESS, std::to_string(audio_clock_ms));
    };
    audio_service_.SetCallbacks(callbacks);
    audio_service_.EnableWakeWordDetection(true);

    // 注册设备状态变化监听器。
    DeviceStatus::AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // 闹钟的恢复、播放、时钟及云端同步钩子统一在 application_alarm.cc 安装。
    InitializeAlarm();

    // 启动时钟定时器，用于更新状态栏。
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // 构造一次本地 MCP 工具表；WebSocket 首连和重连时直接复用。
    McpServer::GetInstance().Initialize();

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
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", "err_pin");
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", "err_reg");
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

    // 立即更新状态栏以显示当前网络状态。
    Ui::UpdateStatusBar(true);

    // 启动串口调试终端；该模块相互独立，启动失败不影响主流程。
    if (!SerialShell::GetInstance().Initialize()) {
        ESP_LOGW(TAG, "Serial shell initialization failed");
    }

    // 初始化阶段必须等 OTA 检查与更新结束后，才能读取激活信息并创建小聆连接。
    // 等待期间仍复用应用事件分发器，保证升级确认按键、UI 调度和网络事件可用。
    constexpr UBaseType_t kApplicationUiPriority = 5;
    vTaskPrioritySet(nullptr, kApplicationUiPriority);
    ESP_LOGI(TAG, "应用/UI 调度优先级: %u",
             static_cast<unsigned>(kApplicationUiPriority));
    if (!WaitForOtaFlowCompletion()) {
        ESP_LOGE(TAG, "OTA 流程失败，停止设备激活和小聆连接");
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", 94);
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

    // 小聆会维持 WebSocket，并建立对时延敏感的 TTS HTTP 流。
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
}

void Application::Run() {
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
        Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", 94);
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
            task();
        }
    }

    if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
        HandleWakeWordDetectedEvent();
    }

    if (bits & MAIN_EVENT_VAD_CHANGE) {
        if (GetDeviceState() == kDeviceStateListening) {
            auto led = Board::GetInstance().GetLed();
            led->OnStateChanged();
        }
    }

    if (bits & MAIN_EVENT_CLOCK_TICK) {
        clock_ticks_++;
        if (alarm_flow_.on_clock_tick) {
            alarm_flow_.on_clock_tick();
        }
        PublishWifiIcon();
        Ui::UpdateStatusBar();

        if (GetDeviceState() == kDeviceStateListening ||
            GetDeviceState() == kDeviceStateSpeaking) {
            LogUplinkDiagnostics("clock");
        }

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
                PlayTone(72);
            }
        }

        // 每 10 秒输出一次调试信息。
        if (clock_ticks_ % 10 == 0) {
            SystemInfo::PrintHeapStats();
        }
    }
}

void Application::LogUplinkDiagnostics(const char* phase) {
    const auto audio = audio_service_.GetUplinkDiagnostics();
    XiaolingUplinkDiagnostics network;
    if (xiaoling_conn_) {
        network = xiaoling_conn_->GetUplinkDiagnostics();
    }
    const uint32_t now_ms = esp_log_timestamp();
    const auto age_ms = [now_ms](uint32_t timestamp_ms) -> uint32_t {
        return timestamp_ms == 0 ? UINT32_MAX : now_ms - timestamp_ms;
    };
    ESP_LOGI(TAG,
             "上行链路[%s]: state=%s, tts=%lu, processor=%d, local_upload=%d, "
             "cloud_upload=%d, session=%d, connected=%d, gen=%lu, "
             "q=encode:%u/send:%u/ws:%u, count=mic:%lu/afe:%lu/deq:%lu/enc:%lu/"
             "notify:%lu/pop:%lu/ws_in:%lu/ws_out:%lu, reject=%lu, "
             "drop=enc:%lu/ws:%lu, "
             "age=mic:%lu/afe:%lu/enc:%lu/notify:%lu/pop:%lums, ws_batch=%lu/%lu, "
             "ws_active=%d(seq=%lu,%ldms), ws_last=%ldms",
             phase != nullptr ? phase : "unknown",
             DeviceStatus::GetStateName(GetDeviceState()),
             static_cast<unsigned long>(active_tts_session_id_),
             audio.processor_running ? 1 : 0, audio.upload_enabled ? 1 : 0,
             network.upload_enabled ? 1 : 0, network.session_active ? 1 : 0,
             network.connected ? 1 : 0, static_cast<unsigned long>(audio.generation),
             static_cast<unsigned>(audio.encode_queue),
             static_cast<unsigned>(audio.send_queue),
             static_cast<unsigned>(network.queue),
             static_cast<unsigned long>(audio.mic_reads),
             static_cast<unsigned long>(audio.afe_frames),
             static_cast<unsigned long>(audio.encode_dequeued),
             static_cast<unsigned long>(audio.encoded_packets),
             static_cast<unsigned long>(audio.send_notifications),
             static_cast<unsigned long>(audio.popped_packets),
             static_cast<unsigned long>(network.enqueued_packets),
             static_cast<unsigned long>(network.sent_frames),
             static_cast<unsigned long>(network.rejected_packets),
             static_cast<unsigned long>(audio.dropped_encode_frames),
             static_cast<unsigned long>(network.dropped_packets),
             static_cast<unsigned long>(age_ms(audio.last_mic_ms)),
             static_cast<unsigned long>(age_ms(audio.last_afe_ms)),
             static_cast<unsigned long>(age_ms(audio.last_encoded_ms)),
             static_cast<unsigned long>(age_ms(audio.last_notification_ms)),
             static_cast<unsigned long>(age_ms(audio.last_popped_ms)),
             static_cast<unsigned long>(network.batches_started),
             static_cast<unsigned long>(network.batches_completed),
             network.send_in_progress ? 1 : 0,
             static_cast<unsigned long>(network.active_sequence),
             static_cast<long>(network.active_send_ms),
             static_cast<long>(network.last_send_completed_ms));
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
    XiaolingConnActivation activation;
    if (!activation.Fetch()) {
        // 激活信息查询不会改写 Flash；请求失败时仍让小聆连接层按自身的
        // 鉴权和重连策略处理，不把它误当成 OTA 失败。
        ESP_LOGW(TAG, "未取得小聆激活信息，继续初始化小聆连接");
        return;
    }

    if (!activation.code().empty()) {
        ShowActivationCode(activation.code(), activation.message(), activation.qrcode());
    }
    if (!activation.IsRequired()) {
        return;
    }

    constexpr int kActivationAttempts = 10;
    for (int attempt = 0; attempt < kActivationAttempts; ++attempt) {
        ESP_LOGI(TAG, "等待设备绑定: %d/%d", attempt + 1, kActivationAttempts);
        const esp_err_t result = activation.Submit();
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "设备绑定成功");
            Ui::DismissQrCode();
            return;
        }

        const int delay_ms = result == ESP_ERR_TIMEOUT ? 3000 : 10000;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    ESP_LOGW(TAG, "启动阶段内设备绑定未完成");
}

void Application::ShowActivationCode(const std::string& code,
                                     const std::string& message,
                                     const std::string& qrcode) {
    const std::string activation_title = message.empty()
                                             ? Lang::Strings::ACTIVATION
                                             : message;
    const std::string activation_text = code.empty()
                                            ? activation_title
                                            : activation_title + "\n" +
                                                  Lang::Strings::ACTIVATION_CODE + code;

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

    Ui::SetStatus(Ui::Status::LoadingProtocol);

    // 小聆只使用 HTTP 鉴权和 WebSocket 会话。
    xiaoling_conn_ = std::make_unique<XiaolingConn>();

#ifdef CONFIG_LSPLATFORM
    {
        Settings settings("wifi", true);
        if (settings.GetBool("narrowband_mode", false)) {
            xiaoling_conn_->SetNarrowbandMode(true);
            audio_service_.SetNarrowbandMode(true);
            narrowband_mode_ = true;
            ESP_LOGI(TAG, "Narrowband mode enabled");
        }
    }
#endif  // 小聆平台

    xiaoling_conn_->OnConnected([this, codec, &board]() {
        DismissAlert();
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
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
            if (xiaoling_conn_->CanUploadAudio()) {
                audio_service_.EnableVoiceProcessing(true);
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Start();
#endif  // CONFIG_LSPLATFORM
            }
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

    xiaoling_conn_->OnIncomingJson([this](const cJSON* root) {
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

        if (strcmp(action->valuestring, "finish") == 0 ||
            strcmp(action->valuestring, "closed") == 0) {
            const bool natural_finish = strcmp(action->valuestring, "finish") == 0;
            Schedule([this, natural_finish]() {
                const bool has_music =
                    intent_manager_.Contains(PlaybackIntentId::kMusic);
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
                    audio_service_.EnableVoiceProcessing(false);
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

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    Ui::SetStatusText(status);
    Ui::SetEmotion(emotion);
    Ui::SetChatMessage("system", message);
    if (!sound.empty()) {
        PlayRespakAudio(std::string(sound).c_str());
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, uint16_t tone_id) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    Ui::SetStatusText(status);
    Ui::SetEmotion(emotion);
    Ui::SetChatMessage("system", message);
    PlayTone(tone_id);
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        Ui::SetStatus(Ui::Status::Standby);
        Ui::SetEmotion("neutral");
        Ui::SetChatMessage("system", "");
    }
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
    if (audio_player_.IsWakeupPending()) {
        CancelWakeup();
        return;
    }
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
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
        audio_service_.EnableVoiceProcessing(false);
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

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
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

void Application::HandleWakeWordDetectedEvent() {
    if (alarm_flow_.on_wake_word) {
        alarm_flow_.on_wake_word();
    }
    if (exit_requested_.load() &&
        (tts_waiting_for_playback_.load() || audio_player_.IsPlaying())) {
        ESP_LOGI(TAG, "忽略退出期间排队的唤醒词，TTS 尚未播放完成");
        audio_service_.EnableWakeWordDetection(false);
        return;
    }
    PauseWakeWordDetection();

    auto state = GetDeviceState();
    if (state != kDeviceStateIdle && state != kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Ignoring wake word while device is in %s state",
                 DeviceStatus::GetStateName(state));
        return;
    }

    if (!xiaoling_conn_) {
        return;
    }

#if CONFIG_SEND_WAKE_WORD_DATA
    audio_service_.EncodeWakeWord();
#endif

    if (state == kDeviceStateSpeaking) {
        // 语音唤醒与按键唤醒走同一条 TTS 打断链路：先冻结 timeline
        // 游标并停止旧回复，再启动下一轮，避免旧 sid 被新 start 覆盖。
        AbortSpeaking(kAbortReasonWakeWordDetected);
        SetDeviceState(kDeviceStateIdle);
    }

    if (!EnsureXiaolingReadyForWakeup()) {
        return;
    }

    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
    xiaoling_conn_->SendWakeWordDetected(wake_word);
#if CONFIG_SEND_WAKE_WORD_DATA
    // lschat 在 started 前不接收音频，客户端会暂存这些唤醒词数据。
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        xiaoling_conn_->SendAudio(std::move(packet));
    }
#endif
    BeginWakeup(listening_mode_);
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
        audio_service_.EnableVoiceProcessing(false);
        session_controller_.EndSession();
        if (xiaoling_conn_) xiaoling_conn_->SendStopListening();
        const bool tts_active = tts_waiting_for_playback_.load() ||
                                active_tts_session_id_ != 0 ||
                                audio_player_.IsPlaying();
        if (tts_active) {
            audio_service_.EnableWakeWordDetection(false);
            ESP_LOGI(TAG, "退出请求：TTS 仍在播放，暂时屏蔽唤醒检测");
        } else {
            tts_output_started_.store(false);
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
        audio_service_.EnableVoiceProcessing(false);
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

void Application::BeginWakeup(ListeningMode mode) {
    if (audio_player_.IsWakeupPending() || !xiaoling_conn_) {
        return;
    }
    intent_manager_.Push(PlaybackIntentId::kVoiceSession);
    exit_requested_.store(false);
    if (!SetDeviceState(kDeviceStateConnecting)) {
        intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
        return;
    }
    iat_utterance_active_ = false;
    audio_service_.EnableVoiceProcessing(false);
    if (!intent_manager_.Push(PlaybackIntentId::kPromptTone)) {
        ESP_LOGE(TAG, "唤醒提示音事件入栈失败");
        SetDeviceState(kDeviceStateIdle);
        intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
        return;
    }
    audio_player_.PlayWakeup([this](bool proceed) {
        intent_manager_.Pop(PlaybackIntentId::kPromptTone);
        if (!proceed) {
            if (GetDeviceState() == kDeviceStateListening && xiaoling_conn_) {
                xiaoling_conn_->SendStopListening();
            }
            if (xiaoling_conn_) {
                xiaoling_conn_->SendWakeWordDetected("");
            }
            SetDeviceState(kDeviceStateIdle);
            intent_manager_.Pop(PlaybackIntentId::kVoiceSession);
            return;
        }
        ESP_LOGI(TAG, "Wakeup tone finished; audio upload already active");
    });
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }
    // 不要让采集串行等待提示音播放完成。播放器会在进入监听状态、打开云端
    // 会话并并行启动麦克风链路时，保护唤醒提示音不被 TTS 抢占。
    ESP_LOGI(TAG, "Wakeup tone started; start audio upload in parallel");
    SetListeningMode(mode);
    audio_player_.SetTtsUrlCallback([this]() {
        const uint32_t generation = tts_url_generation_.fetch_add(1) + 1;
        tts_waiting_for_playback_.store(true);
        tts_output_started_.store(false);
        ESP_LOGI(TAG, "TTS URL 已接纳: generation=%lu",
                 static_cast<unsigned long>(generation));
        if (exit_requested_.load()) {
            // MCP exit 可能与 URL 通知发生竞态；即使 URL 在退出请求之后到达，
            // 也要保持唤醒检测关闭。
            audio_service_.EnableWakeWordDetection(false);
        }
        Schedule([this]() {
            const bool has_music =
                intent_manager_.Contains(PlaybackIntentId::kMusic);
            // 对流式 TTS 而言，URL 到达通常会比首段解码 PCM 早数秒。真正开始
            // 播放前先显示等待表情，kPlaying 事件再恢复讲话表情。
            if (GetDeviceState() == kDeviceStateListening ||
                GetDeviceState() == kDeviceStateSpeaking) {
                Ui::SetStatusText("思考中...");
                Ui::SetEmotion("等待");
            }
            if (session_controller_.policy().mode == DuplexMode::kHalfDuplex &&
                GetDeviceState() == kDeviceStateListening) {
                ESP_LOGI(TAG, "半双工收到 TTS URL，结束当前上行会话");
                audio_service_.EnableVoiceProcessing(false);
                if (xiaoling_conn_) xiaoling_conn_->SendStopListening();
            }
            if (has_music) {
                ESP_LOGI(TAG,
                         "音乐事件存在，收到 TTS URL 后仅停止本地录音，保持云端合成");
                audio_service_.EnableVoiceProcessing(false);
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Stop();
#endif  // CONFIG_LSPLATFORM
                // 全双工下 action:cancel 会终止整个云端会话，其中也包括刚刚
                // 发布 URL 的流式 TTS。这里只停止麦克风生产者；TTS 播放完成
                // 后的统一收尾会调用 EndSession()。
            }
        });
    });
    if (iat_subscription_id_ == 0) {
        auto on_iat_text = [this](const XiaolingEvent& event) {
            /* This callback runs on the connection task before dispatch to the
             * application task. Preserve which TTS existed at that instant. */
            const uint32_t observed_tts_generation = tts_url_generation_.load();
            const uint32_t received_ms = esp_log_timestamp();
            ScheduleUrgent([this, text = event.text, observed_tts_generation,
                            received_ms]() {
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
        iat_subscription_id_ = XiaolingEventBus::GetInstance().Subscribe(
            XIAOLING_IAT_START, on_iat_text);
        iat_update_subscription_id_ = XiaolingEventBus::GetInstance().Subscribe(
            XIAOLING_IAT_UPDATE, on_iat_text);
        iat_end_subscription_id_ = XiaolingEventBus::GetInstance().Subscribe(
            XIAOLING_IAT_END, [this](const XiaolingEvent& event) {
                Schedule([this, text = event.text]() {
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

bool Application::EndListeningSessionForWakeup() {
    ESP_LOGI(TAG, "Wake word detected while recording; end the current session");
    iat_utterance_active_ = false;
    audio_service_.EnableVoiceProcessing(false);
    session_controller_.EndSession();
    if (!xiaoling_conn_ || !xiaoling_conn_->EndSession()) {
        ESP_LOGE(TAG, "Failed to report the current session end before wakeup");
        SetDeviceState(kDeviceStateIdle);
        return false;
    }
    return SetDeviceState(kDeviceStateIdle);
}

void Application::PauseWakeWordDetection() {
    audio_service_.PauseWakeWordDetection();
    audio_service_.EnableWakeWordDetection(true);
}

void Application::CancelWakeup() {
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
    SetDeviceState(kDeviceStateListening);
}

#ifdef CONFIG_LSPLATFORM
void Application::CountWatchdogBites() {
    int32_t now = (int32_t)(esp_timer_get_time() / 1000000);
    last_watchdog_bites_.push_back(now);

    if (last_watchdog_bites_.size() > kBadNetworkSampleCount) {
        last_watchdog_bites_.erase(last_watchdog_bites_.begin(), last_watchdog_bites_.end() - kBadNetworkSampleCount);
    }

    int bites_in_window = 0;
    for (const auto& t : last_watchdog_bites_) {
        if (now - t <= kBadNetworkSampleWindowSec) {
            bites_in_window++;
        }
    }

    ESP_LOGI(TAG, "Watchdog bites in last minute: %d", bites_in_window);

    if (bites_in_window >= kBadNetworkSampleCount && !narrowband_mode_) {
        ESP_LOGW(TAG, "Too many watchdog bites, switch to narrowband mode");
        xiaoling_conn_->SetNarrowbandMode(true);
        audio_service_.SetNarrowbandMode(true);
        Settings settings("wifi", true);
        settings.SetBool("narrowband_mode", true);
        narrowband_mode_ = true;
    }
}
#endif  // 小聆平台

void Application::HandleStateChangedEvent() {
    DeviceState new_state = DeviceStatus::GetState();
    if (new_state != kDeviceStateIdle) {
        keep_voice_processing_on_idle_ = false;
    }
    // 监听现在与唤醒提示音并行启动。在“连接中 -> 监听中”的状态切换期间保留
    // 该提示音，其他无关状态变化仍会将其取消。
    if (new_state != kDeviceStateConnecting &&
        new_state != kDeviceStateListening &&
        audio_player_.IsWakeupPending()) {
        audio_player_.CancelWakeup();
        intent_manager_.Pop(PlaybackIntentId::kPromptTone);
        if (xiaoling_conn_) {
            xiaoling_conn_->SendWakeWordDetected("");
        }
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
            if (keep_voice_processing_on_idle_) {
                keep_voice_processing_on_idle_ = false;
                audio_service_.EnableAudioUpload(false);
            } else {
                audio_service_.EnableVoiceProcessing(false);
            }
            if (exit_requested_.load() &&
                (tts_waiting_for_playback_.load() || audio_player_.IsPlaying())) {
                audio_service_.EnableWakeWordDetection(false);
                ESP_LOGI(TAG, "待命期间等待 TTS 播放完成，保持唤醒检测关闭");
            } else {
                audio_service_.EnableWakeWordDetection(true);
            }
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

            // EnableVoiceProcessing() always opens the upload gate and only
            // starts the processor when needed. This also recovers a processor
            // left running after a transient transport reset disabled upload.
            audio_service_.EnableVoiceProcessing(true);
#ifdef CONFIG_LSPLATFORM
            uplink_watchdog_.Start();
#endif  // 小聆平台

            break;
        case kDeviceStateSpeaking:
            Ui::SetStatus(Ui::Status::Speaking);
            Ui::SetEmotion("说话");

            // 音乐位于语音事件下层时停止录音；其他情况下仅在实时会话有效时
            // 保持 AEC 后的上行链路运行。
            if (intent_manager_.Contains(PlaybackIntentId::kMusic) ||
                !session_controller_.KeepListeningDuringTts()) {
                audio_service_.EnableVoiceProcessing(false);
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Stop();
#endif  // 小聆平台
            }
            // GMF 解码结束后仍需排空 TTS PCM；此时 IsPlaying() 已可能为 false。
            if (active_tts_session_id_ == 0 && !tts_waiting_for_playback_.load() &&
                !audio_player_.IsPlaying()) {
                audio_service_.ResetDecoder();
            } else if (active_tts_session_id_ != 0 && !audio_player_.IsPlaying()) {
                ESP_LOGI(TAG, "TTS 会话仍在排空，保留播放器 PCM 队列: session=%lu",
                         static_cast<unsigned long>(active_tts_session_id_));
            }
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
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
    if (exit_requested_.load() &&
        (tts_waiting_for_playback_.load() || audio_player_.IsPlaying())) {
        ESP_LOGI(TAG, "忽略退出期间的唤醒词：%s", wake_word.c_str());
        audio_service_.EnableWakeWordDetection(false);
        return;
    }
    PauseWakeWordDetection();
    if (!xiaoling_conn_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle || state == kDeviceStateListening) {
        audio_service_.EncodeWakeWord();

        if (!EnsureXiaolingReadyForWakeup()) {
            return;
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        if (state == kDeviceStateListening && !EndListeningSessionForWakeup()) {
            return;
        }
        xiaoling_conn_->SendWakeWordDetected(wake_word);
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // lschat 在 started 前不接收音频，客户端会暂存这些唤醒词数据。
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            xiaoling_conn_->SendAudio(std::move(packet));
        }
#endif
        BeginWakeup(listening_mode_);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        SetDeviceState(kDeviceStateIdle);
        if (EnsureXiaolingReadyForWakeup()) {
            xiaoling_conn_->SendWakeWordDetected(wake_word);
            BeginWakeup(listening_mode_);
        }
    }
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

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            Ui::ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            Ui::ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            session_controller_.SetMode(DuplexMode::kFullDuplex);
            Ui::ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }
        if (aec_mode_ != kAecOnDeviceSide) {
            session_controller_.SetMode(DuplexMode::kHalfDuplex);
        }
        listening_mode_ = session_controller_.policy().mode == DuplexMode::kFullDuplex
                              ? kListeningModeRealtime
                              : kListeningModeAutoStop;
        Ui::SetDuplexMode(session_controller_.policy().mode);

        // AEC 模式变化时关闭音频通道。
        if (xiaoling_conn_ && xiaoling_conn_->IsConnected()) {
            xiaoling_conn_->Disconnect();
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
        const bool voice_processing_running = audio_service_.IsAudioProcessorRunning();

        // 结束当前云端会话并只停止上行链路。保持处理器运行，可以在恢复唤醒词
        // 检测时避免拆除并重建 AFE。
        iat_utterance_active_ = false;
        audio_service_.EnableAudioUpload(false);
        session_controller_.EndSession();
        if (xiaoling_conn_ && !xiaoling_conn_->EndSession()) {
            ESP_LOGW(TAG, "切换交互模式时上报会话结束失败");
        }

        session_controller_.SetMode(mode);
        Ui::SetDuplexMode(mode);
        listening_mode_ = mode == DuplexMode::kFullDuplex
                              ? kListeningModeRealtime
                              : kListeningModeAutoStop;
        audio_service_.EnableDeviceAec(
            mode == DuplexMode::kFullDuplex && aec_mode_ == kAecOnDeviceSide);

        if (state == kDeviceStateConnecting ||
            state == kDeviceStateListening ||
            state == kDeviceStateSpeaking) {
            keep_voice_processing_on_idle_ = voice_processing_running;
            if (!SetDeviceState(kDeviceStateIdle)) {
                keep_voice_processing_on_idle_ = false;
            }
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
    audio_service_.PlaySound(std::string_view(
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
    audio_service_.PlaySound(std::string_view(
        reinterpret_cast<const char*>(resource.address), resource.size));
}

void Application::PlayTone(uint16_t tone_id) {
    if (audio_player_.PlayTone(tone_id) == 0) {
        ESP_LOGW(TAG, "ROMFS tone 播放失败: id=%u", static_cast<unsigned>(tone_id));
    }
}

void Application::ResetXiaolingConn() {
    Schedule([this]() {
        std::lock_guard<std::mutex> lock(xiaoling_conn_mutex_);
        // 如果音频通道已打开，则将其关闭。
        if (xiaoling_conn_ && xiaoling_conn_->IsConnected()) {
            xiaoling_conn_->Disconnect();
        }
        // 重置协议对象。
        xiaoling_conn_.reset();
    });
}
