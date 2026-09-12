#include "application.h"
#include "board.h"
#include "display.h"
#include "system/system_info.h"
#include "audio_codec.h"
#include "xiaoling/xiaoling_conn.h"
#include "i18n/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "system/time_sync.h"
#include "ui_app.h"
#include "ui_manager.h"
#include "status_model.h"
#include "voice_model.h"
#include "wifi_model.h"
#include "audio/tone/tone_model.h"

#ifdef CONFIG_LSPLATFORM
#include "image_fetcher.h"
#endif // CONFIG_LSPLATFORM

#if CONFIG_LSPLATFORM_BANNERS
#include "banner_model.h"
#endif // CONFIG_LSPLATFORM_BANNERS

#include <cstring>
#include <esp_log.h>
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
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();

    // UI 层绑定 display 硬件（页面导航、亮度、banner 轮换等 UI 能力由此进入）
    auto& ui_manager = UiManager::GetInstance();
    ui_manager.Initialize(display);
    ui_manager.SetTaskScheduler([this](std::function<void()>&& task) {
        Schedule(std::move(task));
    });
#if CONFIG_LSPLATFORM_BANNERS
    // BannerModel 负责解析和保存数据，HomePresenter 负责轮换与显示。
    BannerModel::GetInstance().SetWakeWordProvider([this]() {
        return audio_service_.GetLastWakeWord();
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
    ToneModel::GetInstance().Initialize(Assets::GetInstance());
#endif

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    if (!audio_player_.Initialize(&audio_service_)) {
        ESP_LOGE(TAG, "TTS 播放器初始化失败");
    }
    audio_player_.SetEventCallback([this](const PlaybackEvent& event) {
        if (event.channel != PlaybackChannel::kTts) {
            return;
        }
        Schedule([this, event]() {
            if (event.state == PlaybackState::kPlaying) {
                active_tts_session_id_ = event.session_id;
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
            if (GetDeviceState() == kDeviceStateSpeaking) {
                SetDeviceState(kDeviceStateIdle);
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
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    StatusModel::GetInstance().SetText(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
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
                StatusModel::GetInstance().SetText(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", "err_pin");
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", "err_reg");
                break;
            case NetworkEvent::ModemErrorInitFailed:
                StatusModel::GetInstance().SetText(Lang::Strings::DETECTING_MODULE);
                display->SetChatMessage("system", Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorTimeout:
                StatusModel::GetInstance().SetText(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
        WifiModel::GetInstance().Refresh();
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
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
            auto display = Board::GetInstance().GetDisplay();
            WifiModel::GetInstance().Refresh();
            display->UpdateStatusBar();

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
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    audio_player_.Stop(PlaybackEndReason::kCancelled);

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

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

    // OTA 当前只确认运行固件有效并视为最新版本，后续能力统一在 ota/ 中扩展。
    ota_ = std::make_unique<Ota>();
    if (!CheckNewVersion()) {
        ESP_LOGE(TAG, "OTA 启动检查失败，停止连接小聆");
        last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        return;
    }

    // OTA 完成后建立小聆连接；连接成功后语音模块立即就绪。
    InitializeXiaolingConn();
    CheckAssetsVersion();

    // 通知主循环启动流程已完成。
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", "upgrade");
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", 94);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    // 资源 OTA 完成后重新建立 tone 索引，避免继续引用旧 ROMFS 地址。
    ToneModel::GetInstance().Initialize(assets);
    UiApp::ReloadEmojiResources();
#endif
    display->SetChatMessage("system", "");
    VoiceModel::GetInstance().SetEmotion("microchip_ai");
}

bool Application::CheckNewVersion() {
    StatusModel::GetInstance().SetText(Lang::Strings::CHECKING_NEW_VERSION);
    if (ota_ == nullptr || ota_->CheckVersion() != ESP_OK) {
        return false;
    }
    ota_->MarkCurrentVersionValid();
    return true;
}
void Application::InitializeXiaolingConn() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    StatusModel::GetInstance().SetText(Lang::Strings::LOADING_PROTOCOL);

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

    xiaoling_conn_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
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
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
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
        ESP_LOGE(TAG, "小聆连接失败，唤醒时将继续重试");
    }
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
    auto display = Board::GetInstance().GetDisplay();
    StatusModel::GetInstance().SetText(status);
    VoiceModel::GetInstance().SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        PlayLocalizedTone(std::string(sound).c_str());
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, uint16_t tone_id) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    StatusModel::GetInstance().SetText(status);
    VoiceModel::GetInstance().SetEmotion(emotion);
    display->SetChatMessage("system", message);
    PlayTone(tone_id);
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        StatusModel::GetInstance().SetText(Lang::Strings::STANDBY);
        VoiceModel::GetInstance().SetEmotion("neutral");
        display->SetChatMessage("system", "");
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
        if (!xiaoling_conn_->IsConnected()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!xiaoling_conn_->Connect()) {
                return;
            }
        }

        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        xiaoling_conn_->Disconnect();
    }
}

void Application::HandleStartListeningEvent() {
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
        if (!xiaoling_conn_->IsConnected()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!xiaoling_conn_->Connect()) {
                return;
            }
        }

        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
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
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!xiaoling_conn_->IsConnected()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!xiaoling_conn_->Connect()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        xiaoling_conn_->SendWakeWordDetected(wake_word);
#if CONFIG_SEND_WAKE_WORD_DATA
        // lschat 在 started 前不接收音频，客户端会暂存这些唤醒词数据。
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            xiaoling_conn_->SendAudio(std::move(packet));
        }
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        // Set flag to play popup sound after state changes to listening
        // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
        play_popup_on_listening_ = true;
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
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
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            StatusModel::GetInstance().SetText(Lang::Strings::STANDBY);
            VoiceModel::GetInstance().SetEmotion("neutral");
#ifdef CONFIG_LSPLATFORM
            display->DismissImage();
            display->DismissActivation();
#endif // CONFIG_LSPLATFORM
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
#ifdef CONFIG_LSPLATFORM
            uplink_watchdog_.Stop();
            downlink_watchdog_.Stop();
#endif // CONFIG_LSPLATFORM
            break;
        case kDeviceStateConnecting:
            StatusModel::GetInstance().SetText(Lang::Strings::CONNECTING);
            VoiceModel::GetInstance().SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            StatusModel::GetInstance().SetText(Lang::Strings::LISTENING);
            VoiceModel::GetInstance().SetEmotion("neutral");
#ifdef CONFIG_LSPLATFORM
            display->DismissImage();
            display->DismissActivation();
#endif // CONFIG_LSPLATFORM

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                xiaoling_conn_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Start();
#endif // CONFIG_LSPLATFORM
            }

            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                PlayTone(94);
            }
            break;
        case kDeviceStateSpeaking:
            StatusModel::GetInstance().SetText(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Stop();
#endif // CONFIG_LSPLATFORM
            }
            // URL 播放器启动前已经清理旧队列，避免状态回调误删刚解码出的 MP3 PCM。
            if (!audio_player_.IsPlaying()) {
                audio_service_.ResetDecoder();
            }
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
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
    auto display = board.GetDisplay();

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
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [display](int progress, size_t speed) {
        std::thread([display, progress, speed]() {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            display->SetChatMessage("system", buffer);
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
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
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

        if (!xiaoling_conn_->IsConnected()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!xiaoling_conn_->Connect()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        xiaoling_conn_->SendWakeWordDetected(wake_word);
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // lschat 在 started 前不接收音频，客户端会暂存这些唤醒词数据。
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            xiaoling_conn_->SendAudio(std::move(packet));
        }
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        // Set flag to play popup sound after state changes to listening
        // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
        play_popup_on_listening_ = true;
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
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
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (xiaoling_conn_ && xiaoling_conn_->IsConnected()) {
            xiaoling_conn_->Disconnect();
        }
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
