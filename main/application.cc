#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "xiaoling/xiaoling_client.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#ifdef CONFIG_LSPLATFORM
#include "image_fetcher.h"
#endif // CONFIG_LSPLATFORM

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <mbedtls/base64.h>

#define TAG "Application"

namespace {

bool DecodeTtsUrl(const cJSON* data, std::string& url) {
    const auto* content = cJSON_IsObject(data)
                              ? cJSON_GetObjectItemCaseSensitive(data, "content")
                              : nullptr;
    if (!cJSON_IsString(content) || content->valuestring == nullptr || content->valuestring[0] == '\0') {
        return false;
    }

    const auto* encoded = reinterpret_cast<const unsigned char*>(content->valuestring);
    const size_t encoded_size = std::strlen(content->valuestring);
    size_t required_size = 0;
    const int size_result = mbedtls_base64_decode(nullptr, 0, &required_size, encoded, encoded_size);
    if (size_result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || required_size == 0) {
        return false;
    }

    std::vector<unsigned char> decoded(required_size + 1, 0);
    size_t decoded_size = 0;
    if (mbedtls_base64_decode(decoded.data(), required_size, &decoded_size, encoded, encoded_size) != 0 ||
        decoded_size == 0) {
        return false;
    }
    if (decoded[decoded_size - 1] == '\0') {
        --decoded_size;
    }
    if (decoded_size == 0 || std::memchr(decoded.data(), '\0', decoded_size) != nullptr) {
        return false;
    }

    url.assign(reinterpret_cast<const char*>(decoded.data()), decoded_size);
    return true;
}

}  // namespace


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
        if (event.channel != PlaybackChannel::kTts ||
            (event.state != PlaybackState::kCompleted &&
             event.state != PlaybackState::kStopped &&
             event.state != PlaybackState::kFailed)) {
            return;
        }
        Schedule([this, event]() {
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
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
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
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                display->SetChatMessage("system", Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
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
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
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
                if (protocol_ && protocol_->SendAudio(std::move(packet))) {
                    uplink_watchdog_.Feed(duration);
                }
#else // !CONFIG_LSPLATFORM
                if (protocol_) {
                    protocol_->SendAudio(std::move(packet));
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
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
            
#if CONFIG_LSPLATFORM_BANNERS
            if (clock_ticks_ % 5 == 0 && GetDeviceState() == kDeviceStateIdle) {
                auto banners = Board::GetInstance().GetBanners();
                auto banner = banners->Next();
                display->SetChatMessage("system", banner.c_str());
            }
#endif
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

    // 网络断开时关闭预先建立的 WebSocket，避免恢复网络后继续使用失效连接。
    if (protocol_ != nullptr) {
        ESP_LOGI(TAG, "Closing lschat WebSocket due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Play the success sound to indicate the device is ready
    audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
}

void Application::ActivationTask() {
    // OTA 对象仍用于版本检查，但小聆端云鉴权不依赖 OTA 激活流程。
    ota_ = std::make_unique<Ota>();

    // Wi-Fi 就绪后优先完成小聆鉴权，避免 OTA 或旧激活流程阻塞端云连接。
    InitializeCloudClient();

    // 检查资源和固件更新。
    CheckAssetsVersion();
    CheckNewVersion();

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
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
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
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
#ifdef CONFIG_LSPLATFORM
    const int MAX_RETRY = 3;
#else // !CONFIG_LSPLATFORM
    const int MAX_RETRY = 10;
#endif // CONFIG_LSPLATFORM
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
#ifdef CONFIG_LSPLATFORM
                ESP_LOGW(TAG, "OTA version check failed, continue Xiaoling startup");
#endif // CONFIG_LSPLATFORM
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
#ifdef CONFIG_LSPLATFORM
        // 小聆使用独立的 lschat 鉴权，不处理旧 OTA 接口返回的设备激活信息。
        if (ota_->HasActivationCode() || ota_->HasActivationChallenge()) {
            ESP_LOGI(TAG, "Ignoring legacy OTA activation data in Xiaoling mode");
        }
        break;
#else // !CONFIG_LSPLATFORM
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
#ifdef CONFIG_LSPLATFORM
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage(), ota_->GetActivationQRCode());
#else // !CONFIG_LSPLATFORM
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
#endif // CONFIG_LSPLATFORM
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
#ifdef CONFIG_LSPLATFORM
                ESP_LOGI(TAG, "Activation successful, dismiss UI");
                display->DismissActivation();
#endif // CONFIG_LSPLATFORM
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
#endif // CONFIG_LSPLATFORM
    }
}

void Application::InitializeCloudClient() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // 小聆只使用 HTTP 鉴权和 WebSocket 会话。
    protocol_ = std::make_unique<XiaolingClient>();

#ifdef CONFIG_LSPLATFORM
    {
        Settings settings("wifi", true);
        if (settings.GetBool("narrowband_mode", false)) {
            protocol_->SetNarrowbandMode(true);
            audio_service_.SetNarrowbandMode(true);
            narrowband_mode_ = true;
            ESP_LOGI(TAG, "Narrowband mode enabled");
        }
    }
#endif // CONFIG_LSPLATFORM

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
#ifdef CONFIG_LSPLATFORM
        downlink_watchdog_.Feed(packet->frame_duration);
#endif // CONFIG_LSPLATFORM
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            if (!audio_player_.IsPlaying()) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
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

            if (strcmp(sub->valuestring, "iat") == 0) {
                const auto* text = cJSON_GetObjectItemCaseSensitive(data, "text");
                if (cJSON_IsString(text) && text->valuestring != nullptr) {
                    ESP_LOGI(TAG, ">> %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->DismissImage();
                        display->DismissActivation();
                        display->SetChatMessage("user", message.c_str());
                    });
                }
            } else if (strcmp(sub->valuestring, "tts") == 0) {
                std::string tts_url;
                if (!DecodeTtsUrl(data, tts_url)) {
                    ESP_LOGE(TAG, "小聆 TTS URL Base64 解码失败");
                    return;
                }
                ESP_LOGI(TAG, "收到小聆 TTS URL: decoded_bytes=%u",
                         static_cast<unsigned>(tts_url.size()));
                Schedule([this, url = std::move(tts_url)]() {
                    const auto session_id = audio_player_.PlayTtsUrl(url);
                    if (session_id == 0) {
                        ESP_LOGE(TAG, "提交 TTS 流式播放失败");
                        if (GetDeviceState() == kDeviceStateListening ||
                            GetDeviceState() == kDeviceStateSpeaking) {
                            SetDeviceState(kDeviceStateIdle);
                        }
                        return;
                    }

                    active_tts_session_id_ = session_id;
                    if (!SetDeviceState(kDeviceStateSpeaking)) {
                        ESP_LOGE(TAG, "当前设备状态不允许播放 TTS");
                        active_tts_session_id_ = 0;
                        audio_player_.Stop(PlaybackEndReason::kCancelled);
                    }
                });
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
    
    if (protocol_->Start()) {
        ESP_LOGI(TAG, "Xiaoling client authentication ready");

        // 鉴权成功后立即建立长连接。音频会话仍由唤醒流程通过 start 消息开启，
        // 这里只建立 WebSocket 传输层，避免首次唤醒时再等待握手。
        if (!protocol_->IsAudioChannelOpened()) {
            ESP_LOGI(TAG, "Opening lschat WebSocket after authentication");
            if (!protocol_->OpenAudioChannel()) {
                ESP_LOGW(TAG, "Initial lschat WebSocket connection failed; wake-up will retry");
            }
        }
    } else {
        ESP_LOGE(TAG, "Xiaoling client authentication failed");
    }

#if CONFIG_LSPLATFORM_BANNERS
    auto banners = board.GetBanners();
    banners->Fetch(ota_->GetBannersUrl());
#endif
}

void Application::ShowActivationCode(const std::string& code, const std::string& message, const std::string& qrcode) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
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

    audio_service_.PlaySound(Lang::Sounds::OGG_WECHAT_QRCODE);
    audio_service_.PlaySound(Lang::Sounds::OGG_BINDING);
#else // !CONFIG_LSPLATFORM
    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);
#endif // CONFIG_LSPLATFORM
    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
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

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                return;
            }
        }

        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
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

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
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
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        protocol_->SendWakeWordDetected(wake_word);
#if CONFIG_SEND_WAKE_WORD_DATA
        // lschat 在 started 前不接收音频，客户端会暂存这些唤醒词数据。
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
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
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
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
        protocol_->SetNarrowbandMode(true);
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
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
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
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
#ifdef CONFIG_LSPLATFORM
            display->DismissImage();
            display->DismissActivation();
#endif // CONFIG_LSPLATFORM

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Start();
#endif // CONFIG_LSPLATFORM
            }

            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

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
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
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
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
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
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
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
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        protocol_->SendWakeWordDetected(wake_word);
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // lschat 在 started 前不接收音频，客户端会暂存这些唤醒词数据。
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
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
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
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
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
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
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetCloudClient() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

