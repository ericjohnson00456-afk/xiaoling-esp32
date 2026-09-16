#include "application.h"
#include "board.h"
#include "display.h"
#include "system/system_info.h"
#include "audio_codec.h"
#include "xiaoling/conn/xiaoling_conn.h"
#include "ui/i18n/lang_config.h"
#include "mcp_server.h"
#include "resource/assets.h"
#include "settings.h"
#include "serial_shell.h"
#include "system/time_sync.h"
#include "ui/core/ui_app.h"
#include "ui/core/ui_manager.h"
#include "ui/events/ui_events.h"
#include "voice_model.h"
#include "xiaoling/xiaoling_events.h"
#include "wifi_model.h"
#include "player/resource/tone_model.h"

#ifdef CONFIG_LSPLATFORM
#include "image_fetcher.h"
#endif // CONFIG_LSPLATFORM

#if CONFIG_LSPLATFORM_BANNERS
#include "banner_model.h"
#endif // CONFIG_LSPLATFORM_BANNERS

#include <cstring>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"


#ifdef CONFIG_LSPLATFORM
static const int kBadNetworkSampleWindowSec = 60;
static const int kBadNetworkSampleCount = 3;
#endif

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
    session_controller_.SetMode(aec_mode_ == kAecOnDeviceSide
                                   ? DuplexMode::kFullDuplex
                                   : DuplexMode::kHalfDuplex);

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
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return DeviceStatus::TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();

    // Board input is delivered through the default esp_event loop.  This
    // keeps button callbacks in the board layer free from application/UI
    // dependencies and also gives us one place to apply device-state rules.
    ESP_ERROR_CHECK(esp_event_handler_register(
        BOARD_EVENT, ESP_EVENT_ANY_ID, &Application::OnBoardInputEvent, this));
    ESP_ERROR_CHECK(esp_event_handler_register(
        BUTTON_EVENT, ESP_EVENT_ANY_ID, &Application::OnButtonEvent, this));

    SetDeviceState(kDeviceStateStarting);

    // UIApp owns display binding and page initialization; Application only
    // provides the scheduler used to marshal work onto the application task.
    auto& ui_manager = UiManager::GetInstance();
    ui_manager.SetTaskScheduler([this](std::function<void()>&& task) {
        Schedule(std::move(task));
    });
#if CONFIG_LSPLATFORM_BANNERS
    // BannerModel 负责解析和保存数据，HomePresenter 负责轮换与显示。
    BannerModel::GetInstance().SetWakeWordProvider([this]() {
        return audio_service_.GetWakeWordName();
    });
    ui_manager.SetBannerIdleProvider([this]() {
        return GetDeviceState() == kDeviceStateIdle;
    });
#endif // CONFIG_LSPLATFORM_BANNERS
    if (!UiApp::Initialize()) {
        ESP_LOGE(TAG, "Home 页面初始化失败");
    }
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    // 语音播放初始化前建立 ROMFS tone 索引，播放器随后可按 tone_id 读取资源。
    ToneModel::GetInstance().Initialize(Assets::GetInstance().ToneStore(), "/");
#endif

    // Print board name/version info
    UiEvents::PostChatMessage("system", SystemInfo::GetUserAgent());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.EnableDeviceAec(aec_mode_ == kAecOnDeviceSide);
    audio_service_.Start();

    if (!audio_player_.Initialize(&audio_service_)) {
        ESP_LOGE(TAG, "TTS 播放器初始化失败");
    }
    audio_player_.SetEventCallback([this](const PlaybackEvent& event) {
        Schedule([this, event]() {
            audio_player_.DispatchPlaybackEvent(event);
            if (event.channel != PlaybackChannel::kTts) {
                return;
            }
            if (event.state == PlaybackState::kPlaying) {
                active_tts_session_id_ = event.session_id;
                session_controller_.StartTts(event.session_id);
                if (GetDeviceState() == kDeviceStateIdle ||
                    GetDeviceState() == kDeviceStateListening) {
                    SetDeviceState(kDeviceStateSpeaking);
                }
                return;
            }
            if (event.state != PlaybackState::kCompleted &&
                event.state != PlaybackState::kStopped &&
                event.state != PlaybackState::kFailed) {
                return;
            }
            if (active_tts_session_id_ != event.session_id) {
                return;
            }
            active_tts_session_id_ = 0;
            session_controller_.EndTts(event.session_id);
            if (GetDeviceState() == kDeviceStateSpeaking) {
                if (session_controller_.KeepListeningDuringTts()) {
                    SetDeviceState(kDeviceStateListening);
                } else if (event.channel == PlaybackChannel::kTts &&
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
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    DeviceStatus::AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    board.RegisterMcpTools(mcp_server);

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Scanning:
                UiEvents::PostNotification(UiEvents::Notification::ScanningWifi, 30000);
                network_connected_ = false;
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    UiEvents::PostStatus(UiEvents::Status::RegisteringNetwork);
                } else {
                    // WiFi or cellular with carrier info
                    UiEvents::PostNotification(UiEvents::Notification::ConnectingToNetwork, 30000, data);
                }
                break;
            }
            case NetworkEvent::Connected: {
                    UiEvents::PostNotification(UiEvents::Notification::ConnectedToNetwork, 30000, data);
                network_connected_ = true;
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                network_connected_ = false;
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                UiEvents::PostStatus(UiEvents::Status::DetectingModule);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", "err_pin");
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", "err_reg");
                break;
            case NetworkEvent::ModemErrorInitFailed:
                UiEvents::PostStatus(UiEvents::Status::DetectingModule);
                UiEvents::PostChatMessage("system", Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorTimeout:
                UiEvents::PostStatus(UiEvents::Status::RegisteringNetwork);
                break;
        }
        WifiModel::GetInstance().Refresh();
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    UiEvents::PostStatusBarUpdate(true);

    // Serial debug shell (independent module, failure is non-fatal)
    if (!SerialShell::GetInstance().Initialize()) {
        ESP_LOGW(TAG, "Serial shell initialization failed");
    }
}

void Application::OnBoardInputEvent(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    (void)event_base;
    (void)event_id;
    auto* app = static_cast<Application*>(arg);
    if (app == nullptr || event_data == nullptr) {
        return;
    }
    if (event_id == BoardEvents::kInput) {
        const auto event = *static_cast<const BoardEvents::InputEvent*>(event_data);
        app->Schedule([app, event]() { app->HandleBoardInput(event); });
    } else if (event_id == BoardEvents::kWifiConfigHint) {
        const auto event = *static_cast<const BoardEvents::WifiConfigHintEvent*>(event_data);
        app->Schedule([app, event]() { app->HandleBoardWifiConfigHint(event); });
    } else if (event_id == BoardEvents::kWakeWordDetected) {
        const auto event = *static_cast<const BoardEvents::WakeWordEvent*>(event_data);
        app->Schedule([app, event]() { app->HandleBoardWakeWord(event); });
    } else if (event_id == BoardEvents::kSystemMessage) {
        const auto event = *static_cast<const BoardEvents::SystemMessageEvent*>(event_data);
        app->Schedule([app, event]() { app->HandleBoardSystemMessage(event); });
    } else if (event_id == BoardEvents::kEmotion) {
        const auto event = *static_cast<const BoardEvents::EmotionEvent*>(event_data);
        app->Schedule([app, event]() { app->HandleBoardEmotion(event); });
    } else if (event_id == BoardEvents::kLocalizedText) {
        const auto event = *static_cast<const BoardEvents::LocalizedTextEvent*>(event_data);
        app->Schedule([app, event]() { app->HandleBoardLocalizedText(event); });
    }
}

void Application::HandleBoardWifiConfigHint(const BoardEvents::WifiConfigHintEvent& event) {
    UiEvents::PostWifiConfigHint(event.ssid, event.url);
    PlayTone(70);
}

void Application::HandleBoardWakeWord(const BoardEvents::WakeWordEvent& event) {
    WakeWordInvoke(event.wake_word);
}

void Application::HandleBoardSystemMessage(const BoardEvents::SystemMessageEvent& event) {
    UiEvents::PostChatMessage(event.role, event.content);
}

void Application::HandleBoardEmotion(const BoardEvents::EmotionEvent& event) {
    UiEvents::PostEmotion(event.emotion);
}

void Application::HandleBoardLocalizedText(const BoardEvents::LocalizedTextEvent& event) {
    const auto key = event.key == BoardEvents::LocalizedText::Error
                         ? UiEvents::TextKey::Error
                         : UiEvents::TextKey::PleaseWait;
    UiEvents::PostLocalizedText(key);
}

void Application::HandleBoardInput(const BoardEvents::InputEvent& event) {
    auto& board = Board::GetInstance();
    switch (event.input) {
        case BoardEvents::Input::PrimaryClick:
            if (GetDeviceState() == kDeviceStateStarting && board.EnterNetworkConfigMode()) {
                return;
            }
            ToggleChatState();
            break;
        case BoardEvents::Input::PrimaryDoubleClick:
            if (GetDeviceState() == kDeviceStateStarting ||
                GetDeviceState() == kDeviceStateWifiConfiguring) {
                board.SwitchNetworkType();
            }
            break;
        case BoardEvents::Input::TouchDown:
            StartListening();
            break;
        case BoardEvents::Input::TouchUp:
            StopListening();
            break;
        case BoardEvents::Input::VolumeUp:
        case BoardEvents::Input::VolumeDown:
            UiEvents::PostVolume(event.value);
            break;
        case BoardEvents::Input::VolumeUpLongPress:
            UiEvents::PostVolumeMax();
            break;
        case BoardEvents::Input::VolumeDownLongPress:
            UiEvents::PostVolumeMuted();
            break;
        case BoardEvents::Input::PowerSaveOn:
            UiEvents::PostPowerSaveMode(true);
            break;
        case BoardEvents::Input::PowerSaveOff:
            UiEvents::PostPowerSaveMode(false);
            break;
        case BoardEvents::Input::StatusBarRefresh:
            UiEvents::PostStatusBarUpdate(event.value != 0);
            break;
        case BoardEvents::Input::NetworkSwitchTo4G:
            UiEvents::PostNotification(UiEvents::Notification::SwitchTo4G);
            break;
        case BoardEvents::Input::NetworkSwitchToWifi:
            UiEvents::PostNotification(UiEvents::Notification::SwitchToWifi);
            break;
        case BoardEvents::Input::RebootRequested:
            vTaskDelay(pdMS_TO_TICKS(1000));
            Reboot();
            break;
        case BoardEvents::Input::WifiConfigNotification:
            UiEvents::PostNotification(UiEvents::Notification::EnteringWifiConfigMode);
            break;
        case BoardEvents::Input::EnterWifiConfigState:
            SetDeviceState(kDeviceStateWifiConfiguring);
            break;
        case BoardEvents::Input::EnterIdleState:
            SetDeviceState(kDeviceStateIdle);
            break;
        case BoardEvents::Input::ResetProtocolRequested:
            ResetXiaolingConn();
            break;
        case BoardEvents::Input::AecToggle:
            SetAecMode(aec_mode_ == kAecOff ? kAecOnDeviceSide : kAecOff);
            break;
        case BoardEvents::Input::AecSet:
            SetAecMode(static_cast<AecMode>(event.value));
            break;
        case BoardEvents::Input::PlayToneRequested:
            PlayTone(static_cast<uint16_t>(event.value));
            break;
        case BoardEvents::Input::VolumeChanged:
            UiEvents::PostVolume(event.value, event.percent);
            break;
        case BoardEvents::Input::VolumeMax:
            UiEvents::PostVolumeMax();
            break;
        case BoardEvents::Input::VolumeMuted:
            UiEvents::PostVolumeMuted();
            break;
    }
}

void Application::Run() {
    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

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

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
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
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            // 每轮只发送一个音频包，避免持续录音时饿死 IAT 字幕等主任务事件。
            if (auto packet = audio_service_.PopPacketFromSendQueue()) {
#ifdef CONFIG_LSPLATFORM
                auto duration = packet->frame_duration;
                if (xiaoling_conn_ && xiaoling_conn_->SendAudio(std::move(packet))) {
                    uplink_watchdog_.Feed(duration);
                }
#else // !CONFIG_LSPLATFORM
                if (xiaoling_conn_) {
                    xiaoling_conn_->SendAudio(std::move(packet));
                }
#endif // CONFIG_LSPLATFORM
            }

            // EventGroup 位会合并；队列仍有积压时主动安排下一轮发送。
            if (audio_service_.HasPendingSendPackets()) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
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
            WifiModel::GetInstance().Refresh();
            UiEvents::PostStatusBarUpdate();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    UiEvents::PostStatusBarUpdate(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    audio_player_.Stop(PlaybackEndReason::kCancelled);

    // Update the status bar immediately to show the network state
    UiEvents::PostStatusBarUpdate(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    UiEvents::PostNotificationText(message);
    UiEvents::PostChatMessage("system", "");

    // Play the success sound to indicate the device is ready
    PlayTone(59);

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
}

void Application::ActivationTask() {
    // 网络就绪后先校准系统时间，鉴权签名不再自行发起 SNTP。
    if (!TimeSync::SyncSystemTime()) {
        ESP_LOGE(TAG, "系统校时失败，停止本次启动链路");
        last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        return;
    }

    // Wi-Fi 和 SNTP 就绪后立即建立小聆长连接，连接由独立任务持续维护。
    InitializeXiaolingConn();

    // OTA 当前只确认运行固件有效并视为最新版本，后续能力统一在 ota/ 中扩展。
    ota_ = std::make_unique<Ota>();
    if (!CheckNewVersion()) {
        ESP_LOGE(TAG, "OTA 启动检查失败，停止连接小聆");
        last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        return;
    }

    CheckAssetsVersion();

    // 通知主循环启动流程已完成。
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto& assets = Assets::GetInstance();
    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Resource partitions are unavailable for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    struct PendingResource {
        Assets::Resource resource;
        const char* key;
        const char* alias_key;
        const char* label;
    };
    const PendingResource pending[] = {
        {Assets::Resource::Respak, "respak_url", "download_url", "respak"},
        {Assets::Resource::Tone, "tone_url", nullptr, "tone"},
        {Assets::Resource::Emoji, "emoji_url", nullptr, "emoji"},
        {Assets::Resource::WakeWord, "wake_word_url", "wakeword_url", "wake_word"},
    };

    bool downloaded = false;
    bool failed = false;
    for (const auto& item : pending) {
        std::string url = settings.GetString(item.key);
        if (url.empty() && item.alias_key != nullptr) {
            url = settings.GetString(item.alias_key);
        }
        if (url.empty()) {
            continue;
        }
        settings.EraseKey(item.key);
        if (item.alias_key != nullptr) settings.EraseKey(item.alias_key);

        ESP_LOGI(TAG, "Downloading %s resource from %s", item.label, url.c_str());
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        const bool success = assets.DownloadResource(item.resource, std::move(url),
            [](int progress, size_t speed) {
                std::thread([progress, speed]() {
                    char buffer[48];
                    snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress,
                             static_cast<unsigned>(speed / 1024));
                    UiEvents::PostChatMessage("system", buffer);
                }).detach();
            });
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        if (!success) {
            ESP_LOGE(TAG, "Failed to update %s resource", item.label);
            failed = true;
            continue;
        }
        downloaded = true;
    }

    if (failed && !downloaded) {
        Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED,
              "circle_xmark", 94);
        SetDeviceState(kDeviceStateActivating);
        return;
    }
    if (downloaded) {
        // Tone/emoji/wake-word models may still be referenced by running tasks;
        // reboot after the transaction so every consumer remaps a new image.
        UiEvents::PostChatMessage("system", Lang::Strings::PLEASE_WAIT);
        vTaskDelay(pdMS_TO_TICKS(500));
        Reboot();
        return;
    }

    assets.Apply();
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    ToneModel::GetInstance().Initialize(assets.ToneStore(), "/");
    UiApp::ReloadEmojiResources();
#endif
    UiEvents::PostChatMessage("system", "");
    UiEvents::PostEmotion("microchip_ai");
}

bool Application::CheckNewVersion() {
    UiEvents::PostStatus(UiEvents::Status::CheckingNewVersion);
    if (ota_ == nullptr || ota_->CheckVersion() != ESP_OK) {
        return false;
    }
    ota_->MarkCurrentVersionValid();
    return true;
}
void Application::InitializeXiaolingConn() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    UiEvents::PostStatus(UiEvents::Status::LoadingProtocol);

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
#endif // CONFIG_LSPLATFORM

    xiaoling_conn_->OnConnected([this, codec, &board]() {
        DismissAlert();
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (xiaoling_conn_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "服务端采样率 %d 与设备输出采样率 %d 不一致",
                     xiaoling_conn_->server_sample_rate(), codec->output_sample_rate());
        }
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
#endif // CONFIG_LSPLATFORM
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    xiaoling_conn_->OnDisconnected([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            UiEvents::PostChatMessage("system", "");
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
#if CONFIG_LSPLATFORM_BANNERS
                // 设置推送里的 theme.frontend.banner 由 BannerModel 解析并发布给 UI
                BannerModel::GetInstance().HandleSettingMessage(data);
#endif
            }
            return;
        }

        if (strcmp(action->valuestring, "finish") == 0 ||
            strcmp(action->valuestring, "closed") == 0) {
            Schedule([this]() {
                if (GetDeviceState() == kDeviceStateListening) {
                    SetDeviceState(kDeviceStateIdle);
                }
            });
        } else if (strcmp(action->valuestring, "error") == 0) {
            const auto* code = cJSON_GetObjectItemCaseSensitive(root, "code");
            ESP_LOGE(TAG, "lschat session error: %s",
                     cJSON_IsString(code) && code->valuestring != nullptr ? code->valuestring : "unknown");
        }
    });

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

void Application::ShowActivationCode(const std::string& code, const std::string& message, const std::string& qrcode) {
    struct digit_sound {
        char digit;
        const char* sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', "0"}, digit_sound{'1', "1"}, digit_sound{'2', "2"},
        digit_sound{'3', "3"}, digit_sound{'4', "4"}, digit_sound{'5', "5"},
        digit_sound{'6', "6"}, digit_sound{'7', "7"}, digit_sound{'8', "8"},
        digit_sound{'9', "9"}
    }};

#ifdef CONFIG_LSPLATFORM
    ESP_LOGI(TAG, "Showing activation code: %s, QR url: %s", code.c_str(), qrcode.c_str());
    if (!qrcode.empty()) {
        auto& board = Board::GetInstance();
        auto fetcher = board.GetImageFetcher();
        auto display = board.GetDisplay();
        auto image = fetcher->Fetch(qrcode);
        if (!image) {
            ESP_LOGE(TAG, "Failed to fetch QR code image");
            return;
        }
        display->ShowActivation(std::move(image), message + code);
    } else {
        ESP_LOGW(TAG, "QR code URL is empty, skipping image download");
    }

    PlayTone(71);
#else // !CONFIG_LSPLATFORM
    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", "activation");
#endif // CONFIG_LSPLATFORM
    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            PlayLocalizedTone(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    UiEvents::PostStatusText(status);
    UiEvents::PostEmotion(emotion);
    UiEvents::PostChatMessage("system", message);
    if (!sound.empty()) {
        PlayLocalizedTone(std::string(sound).c_str());
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, uint16_t tone_id) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    UiEvents::PostStatusText(status);
    UiEvents::PostEmotion(emotion);
    UiEvents::PostChatMessage("system", message);
    PlayTone(tone_id);
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        UiEvents::PostStatus(UiEvents::Status::Standby);
        UiEvents::PostEmotion("neutral");
        UiEvents::PostChatMessage("system", "");
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

        BeginWakeup(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        xiaoling_conn_->SendStopListening();
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
        AbortSpeaking(kAbortReasonNone);
        SetDeviceState(kDeviceStateIdle);
        BeginWakeup(kListeningModeManualStop);
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
    if (!xiaoling_conn_) {
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
#if CONFIG_SEND_WAKE_WORD_DATA
        audio_service_.EncodeWakeWord();
#endif

        if (!EnsureXiaolingReadyForWakeup()) {
            audio_service_.EnableWakeWordDetection(true);
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
        BeginWakeup(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    } else if (state == kDeviceStateSpeaking) {
        if (session_controller_.policy().allow_barge_in) {
            AbortSpeaking(kAbortReasonWakeWordDetected);
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
    audio_service_.EnableWakeWordDetection(true);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    audio_player_.Stop(reason == kAbortReasonWakeWordDetected
                           ? PlaybackEndReason::kPreempted
                           : PlaybackEndReason::kCancelled);
    if (xiaoling_conn_) {
        xiaoling_conn_->SendAbortSpeaking(reason);
    }
}

void Application::OnButtonEvent(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    (void)event_base;
    (void)event_data;
    auto* app = static_cast<Application*>(arg);
    if (app != nullptr) {
        app->Schedule([app, event_id]() { app->HandleButtonEvent(event_id); });
    }
}

void Application::HandleButtonEvent(int32_t event_id) {
    if (event_id == ButtonEvents::kToggleAec) {
        if (GetDeviceState() == kDeviceStateIdle) {
            HandleBoardInput({BoardEvents::Input::AecToggle, 0, false});
        }
        return;
    }
    switch (event_id) {
        case ButtonEvents::kClick:
            HandleBoardInput({BoardEvents::Input::PrimaryClick, 0, false});
            break;
        case ButtonEvents::kDoubleClick:
            HandleBoardInput({BoardEvents::Input::PrimaryDoubleClick, 0, false});
            break;
        case ButtonEvents::kTripleClick: {
            auto mode = GetDuplexMode();
            DuplexMode next = mode == DuplexMode::kSimplex ? DuplexMode::kHalfDuplex :
                              mode == DuplexMode::kHalfDuplex ? DuplexMode::kFullDuplex :
                                                               DuplexMode::kSimplex;
            if (next == DuplexMode::kFullDuplex && !audio_service_.SupportsAec()) {
                next = DuplexMode::kSimplex;
            }
            SetDuplexMode(next);
            break;
        }
        case ButtonEvents::kPressDown:
            HandleBoardInput({BoardEvents::Input::TouchDown, 0, false});
            break;
        case ButtonEvents::kPressUp:
            HandleBoardInput({BoardEvents::Input::TouchUp, 0, false});
            break;
        case ButtonEvents::kFactoryReset:
            StartFactoryReset();
            break;
    }
}

void Application::StartFactoryReset() {
    if (factory_reset_started_.exchange(true)) {
        return;
    }
    ESP_LOGW(TAG, "检测到连续按键 8 次，开始恢复出厂设置");
    // 擦除 NVS 后各模块已打开的句柄失效，流程在独立任务里完成后直接复位
    if (xTaskCreate([](void* arg) {
            static_cast<Application*>(arg)->FactoryResetTask();
        }, "factory_reset", 4096, this, 5, nullptr) != pdTRUE) {
        factory_reset_started_ = false;
        ESP_LOGE(TAG, "Failed to create factory reset task");
    }
}

void Application::FactoryResetTask() {
    PlayTone(103);  // 103_reset_factory.mp3
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) {
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "擦除 NVS 失败: %s", esp_err_to_name(err));
    }
    // PlayTone 是异步请求，先等播放真正开始再轮询等结束；起始等待有上限，
    // 避免提示音缺失或播放器异常时卡死
    for (int i = 0; i < 50 && !audio_player_.IsPlaying(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (audio_player_.IsPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGW(TAG, "恢复出厂设置完成，重启设备");
    esp_restart();
}

void Application::BeginWakeup(ListeningMode mode) {
    if (audio_player_.IsWakeupPending() || !xiaoling_conn_) {
        return;
    }
    if (!SetDeviceState(kDeviceStateConnecting)) {
        return;
    }
    audio_service_.EnableVoiceProcessing(false);
    audio_player_.PlayWakeup([this, mode](bool proceed) {
        if (GetDeviceState() != kDeviceStateConnecting) {
            return;
        }
        if (proceed && xiaoling_conn_ && xiaoling_conn_->IsConnected()) {
            ESP_LOGI(TAG, "Wakeup tone finished; start audio upload");
            SetListeningMode(mode);
        } else {
            if (xiaoling_conn_) {
                xiaoling_conn_->SendWakeWordDetected("");
            }
            SetDeviceState(kDeviceStateIdle);
        }
    });
    audio_player_.SetTtsUrlCallback([this]() {
        Schedule([this]() {
            if (session_controller_.policy().mode == DuplexMode::kHalfDuplex &&
                GetDeviceState() == kDeviceStateListening) {
                ESP_LOGI(TAG, "半双工收到 TTS URL，结束当前上行会话");
                audio_service_.EnableVoiceProcessing(false);
                if (xiaoling_conn_) xiaoling_conn_->SendStopListening();
            }
        });
    });
    iat_subscription_id_ = XiaolingEventBus::GetInstance().Subscribe(
        XIAOLING_IAT_START, [this](const XiaolingEvent&) {
            Schedule([this]() {
                if (session_controller_.policy().allow_barge_in && audio_player_.IsPlaying()) {
                    // In full duplex, keep the cloud session and stop only the
                    // local TTS output when the server recognizes barge-in.
                    audio_player_.Stop(PlaybackEndReason::kPreempted);
                }
            });
        });
}

void Application::CancelWakeup() {
    audio_player_.CancelWakeup();
    if (xiaoling_conn_) {
        xiaoling_conn_->SendWakeWordDetected("");
    }
    SetDeviceState(kDeviceStateIdle);
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
#endif // CONFIG_LSPLATFORM

void Application::HandleStateChangedEvent() {
    DeviceState new_state = DeviceStatus::GetState();
    if (new_state != kDeviceStateConnecting && audio_player_.IsWakeupPending()) {
        audio_player_.CancelWakeup();
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
            session_controller_.EndSession();
            UiEvents::PostStatus(UiEvents::Status::Standby);
            UiEvents::PostEmotion("neutral");
#ifdef CONFIG_LSPLATFORM
            UiEvents::PostDismissOverlays();
#endif // CONFIG_LSPLATFORM
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
#ifdef CONFIG_LSPLATFORM
            uplink_watchdog_.Stop();
            downlink_watchdog_.Stop();
#endif // CONFIG_LSPLATFORM
            break;
        case kDeviceStateConnecting:
            // 唤醒提示音阶段的过渡态：连接已就绪，直接预告聆听，避免
            // "连接中..." 一闪而过。
            UiEvents::PostStatus(UiEvents::Status::Listening);
            UiEvents::PostEmotion("neutral");
            UiEvents::PostChatMessage("system", "");
            break;
        case kDeviceStateListening:
            session_controller_.StartListening(listening_mode_ == kListeningModeRealtime);
            UiEvents::PostStatus(UiEvents::Status::Listening);
            UiEvents::PostEmotion("wakeup");
#ifdef CONFIG_LSPLATFORM
            UiEvents::PostDismissOverlays();
#endif // CONFIG_LSPLATFORM

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                xiaoling_conn_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Start();
#endif // CONFIG_LSPLATFORM
            }

            break;
        case kDeviceStateSpeaking:
            UiEvents::PostStatus(UiEvents::Status::Speaking);
            UiEvents::PostEmotion("speaking");

            if (listening_mode_ != kListeningModeRealtime ||
                session_controller_.policy().mode == DuplexMode::kSimplex) {
                audio_service_.EnableVoiceProcessing(false);
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Stop();
#endif // CONFIG_LSPLATFORM
            }
            // URL 播放器启动前已经清理旧队列，避免状态回调误删刚解码出的 MP3 PCM。
            if (!audio_player_.IsPlaying()) {
                audio_service_.ResetDecoder();
            }
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            break;
        default:
            // Do nothing
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


void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (xiaoling_conn_ && xiaoling_conn_->IsConnected()) {
        xiaoling_conn_->Disconnect();
    }
    xiaoling_conn_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (xiaoling_conn_ && xiaoling_conn_->IsConnected()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        xiaoling_conn_->Disconnect();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", "upgrade");
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    UiEvents::PostChatMessage("system", message);

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [](int progress, size_t speed) {
        std::thread([progress, speed]() {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            UiEvents::PostChatMessage("system", buffer);
        }).detach();
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", 94);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        UiEvents::PostChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!xiaoling_conn_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!EnsureXiaolingReadyForWakeup()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        xiaoling_conn_->SendWakeWordDetected(wake_word);
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // lschat 在 started 前不接收音频，客户端会暂存这些唤醒词数据。
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            xiaoling_conn_->SendAudio(std::move(packet));
        }
#endif
        BeginWakeup(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (xiaoling_conn_) {
                xiaoling_conn_->Disconnect();
            }
        });
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

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
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
            UiEvents::PostNotificationText(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            UiEvents::PostNotificationText(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            session_controller_.SetMode(DuplexMode::kFullDuplex);
            UiEvents::PostNotificationText(Lang::Strings::RTC_MODE_ON);
            break;
        }
        if (aec_mode_ != kAecOnDeviceSide) {
            session_controller_.SetMode(DuplexMode::kHalfDuplex);
        }

        // If the AEC mode is changed, close the audio channel
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
        if (GetDeviceState() == kDeviceStateListening ||
            GetDeviceState() == kDeviceStateSpeaking) {
            if (xiaoling_conn_) xiaoling_conn_->SendAbortSpeaking(kAbortReasonNone);
            audio_service_.EnableVoiceProcessing(false);
            SetDeviceState(kDeviceStateIdle);
        }
        session_controller_.SetMode(mode);
        listening_mode_ = mode == DuplexMode::kFullDuplex
                              ? kListeningModeRealtime
                              : kListeningModeAutoStop;
        if (mode == DuplexMode::kFullDuplex && aec_mode_ == kAecOnDeviceSide) {
            audio_service_.EnableDeviceAec(true);
        } else if (aec_mode_ != kAecOnDeviceSide) {
            audio_service_.EnableDeviceAec(false);
        }
        const char* name = mode == DuplexMode::kSimplex ? "simplex" :
                           mode == DuplexMode::kHalfDuplex ? "half-duplex" : "full-duplex";
        ESP_LOGI(TAG, "交互模式切换为 %s", name);
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlayLocalizedTone(const char* name) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (name == nullptr || !ToneModel::GetInstance().GetLocalizedData(name, &data, &size)) {
        ESP_LOGW(TAG, "本地化提示音不存在: %s", name != nullptr ? name : "(null)");
        return;
    }
    audio_service_.PlaySound(std::string_view(reinterpret_cast<const char*>(data), size));
}

void Application::PlayTone(uint16_t tone_id) {
    if (audio_player_.PlayTone(tone_id) == 0) {
        ESP_LOGW(TAG, "ROMFS tone 播放失败: id=%u", static_cast<unsigned>(tone_id));
    }
}

void Application::ResetXiaolingConn() {
    Schedule([this]() {
        // Close audio channel if opened
        if (xiaoling_conn_ && xiaoling_conn_->IsConnected()) {
            xiaoling_conn_->Disconnect();
        }
        // Reset protocol
        xiaoling_conn_.reset();
    });
}
