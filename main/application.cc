#include "application.h"
#include "board.h"
#include "display/display.h"
#include "system/system_info.h"
#include "audio_codec.h"
#include "xiaoling/conn/xiaoling_conn.h"
#include "ui/i18n/lang_config.h"
#include "mcp_server.h"
#include "resource/assets.h"
#include "settings.h"
#include "serial_shell.h"
#include "system/time_sync.h"
#include "ui/ui_app.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"
#include "ui/screens/qrcode/qrcode_presenter.h"
#include "xiaoling/xiaoling_events.h"
#include "player/resource/tone_model.h"
#include "player/resource/localized_audio.h"
#include "shell/cmd/kv/kv_user.h"
#include "ui/screens/home/home_presenter.h"

#ifdef CONFIG_LSPLATFORM
#include "image_fetcher.h"
#endif // CONFIG_LSPLATFORM

#include <cstring>
#include <array>
#include <cstdio>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <font_awesome.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

namespace {

std::string FormatFirmwareSize(size_t size) {
    char text[32] = {};
    if (size >= 1024U * 1024U) {
        std::snprintf(text, sizeof(text), "%.1f MB",
                      static_cast<double>(size) / (1024.0 * 1024.0));
    } else if (size >= 1024U) {
        std::snprintf(text, sizeof(text), "%u KB",
                      static_cast<unsigned>(size / 1024U));
    } else {
        std::snprintf(text, sizeof(text), "%u B", static_cast<unsigned>(size));
    }
    return text;
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

bool Application::SetDeviceState(DeviceState state) {
    return DeviceStatus::TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();

    SetDeviceState(kDeviceStateStarting);

    // Load ROMFS fonts before constructing any UI pages. The display creates
    // its themes during board initialization; this call also verifies that
    // every text and icon font comes from the respak partition.
    if (!Assets::GetInstance().LoadFonts()) {
        ESP_LOGE(TAG, "Required ROMFS font resources are unavailable; no font fallback is enabled");
    }

    // UIApp owns display binding and page initialization; Application only
    // provides the scheduler used to marshal work onto the application task.
    auto& ui_manager = UiManager::GetInstance();
    ui_manager.SetTaskScheduler([this](std::function<void()>&& task) {
        Schedule(std::move(task));
    });
#if CONFIG_LSPLATFORM_BANNERS
    HomePresenter::GetInstance().SetWakeWordProvider([this]() {
        return audio_service_.GetWakeWordName();
    });
    UiApp::SetBannerIdleProvider([this]() {
        return GetDeviceState() == kDeviceStateIdle;
    });
#endif // CONFIG_LSPLATFORM_BANNERS
    if (!UiApp::Initialize()) {
        ESP_LOGE(TAG, "Home 页面初始化失败");
    }
    // Show the provisioning gear until the network and activation checks are
    // complete. The protocol loading step switches to the AI icon before WS.
    UiApp::SetEmotion("gear");
    PublishWifiIcon();
    UiApp::SetDuplexMode(session_controller_.policy().mode);
    auto& initial_assets = Assets::GetInstance();
    LocalizedAudio::GetInstance().Initialize(initial_assets.RespakStore());
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    // 语音播放初始化前建立 ROMFS tone 索引，播放器随后可按 tone_id 读取资源。
    ToneModel::GetInstance().Initialize(initial_assets.ToneStore(), "/");
#endif

    // Print board name/version info
    UiApp::SetChatMessage("system", SystemInfo::GetUserAgent());

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
                if (!exit_requested_.load() && tts_output_started_.exchange(false) &&
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
            if (active_tts_session_id_ != event.session_id) {
                return;
            }
            active_tts_session_id_ = 0;
            session_controller_.EndTts(event.session_id);
            if (exit_requested_.load()) {
                // Exit ends the conversation lifecycle, but the current TTS
                // stream is allowed to drain. Only then restore wakeup.
                ESP_LOGI(TAG, "退出请求：TTS 播放结束，恢复唤醒检测但保持待命");
                if (GetDeviceState() != kDeviceStateIdle) {
                    SetDeviceState(kDeviceStateIdle);
                }
                audio_service_.EnableWakeWordDetection(false);
                xEventGroupClearBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
                audio_service_.EnableWakeWordDetection(true);
                exit_requested_.store(false);
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
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
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
                UiApp::ShowNotification(UiApp::Notification::ScanningWifi, 30000);
                network_connected_ = false;
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    UiApp::SetStatus(UiApp::Status::RegisteringNetwork);
                } else {
                    // WiFi or cellular with carrier info
                    UiApp::ShowNotification(UiApp::Notification::ConnectingToNetwork, 30000, data);
                }
                break;
            }
            case NetworkEvent::Connected: {
                UiApp::ShowNotification(UiApp::Notification::ConnectedToNetwork, 30000, data);
                network_connected_ = true;
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                network_connected_ = false;
                QrCodePresenter::GetInstance().ClearRoleConfigUrl();
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
                UiApp::SetStatus(UiApp::Status::DetectingModule);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", "err_pin");
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", "err_reg");
                break;
            case NetworkEvent::ModemErrorInitFailed:
                UiApp::SetStatus(UiApp::Status::DetectingModule);
                UiApp::SetChatMessage("system", Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorTimeout:
                UiApp::SetStatus(UiApp::Status::RegisteringNetwork);
                break;
        }
        PublishWifiIcon();
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    UiApp::UpdateStatusBar(true);

    // Serial debug shell (independent module, failure is non-fatal)
    if (!SerialShell::GetInstance().Initialize()) {
        ESP_LOGW(TAG, "Serial shell initialization failed");
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
            PublishWifiIcon();
            UiApp::UpdateStatusBar();

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
        // HTTPS OTA uses a deeper TLS/HTTP call stack than the normal
        // activation checks, so leave headroom for the user-confirmed app
        // upgrade that runs in this task.
        }, "activation", 4096 * 3, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    UiApp::UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    audio_player_.Stop(PlaybackEndReason::kCancelled);

    // Update the status bar immediately to show the network state
    UiApp::UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    UiApp::ShowNotification(message);
    UiApp::SetChatMessage("system", "");

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

    auto& board = Board::GetInstance();
    ota_ = std::make_unique<Ota>();
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging) &&
        discharging && battery_level < 10) {
        ESP_LOGW(TAG, "Skip OTA on battery at %d%%", battery_level);
        PlayOtaTone(93);
        InitializeXiaolingConn();
        xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
        return;
    }

    // 只有 SNTP 完成后才请求 OTA；WebSocket 必须等 APP 和资源检查结束后再建立。
    if (!CheckNewVersion()) {
        ESP_LOGE(TAG, "OTA 启动检查失败，停止连接小聆");
        last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        return;
    }

    // Wi-Fi provisioning and device binding are two separate steps. The
    // activation data is returned by the OTA endpoint after AP/BluFi
    // provisioning, so keep the QR/code visible until the server confirms it.
    if (ota_->HasActivationCode() || ota_->HasActivationChallenge()) {
        if (ota_->HasActivationCode()) {
#ifdef CONFIG_LSPLATFORM
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage(),
                               ota_->GetActivationQRCode());
#else
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
#endif
        }

        if (ota_->HasActivationChallenge()) {
            constexpr int kActivationAttempts = 10;
            bool activated = false;
            for (int attempt = 0; attempt < kActivationAttempts; ++attempt) {
                if (GetDeviceState() != kDeviceStateActivating) {
                    break;
                }

                ESP_LOGI(TAG, "Waiting for device binding: %d/%d",
                         attempt + 1, kActivationAttempts);
                const esp_err_t result = ota_->Activate();
                if (result == ESP_OK) {
                    activated = true;
                    break;
                }

                // HTTP 202 means the user has not completed binding yet.
                // Retry at a short interval while leaving the QR code visible.
                const int delay_ms = result == ESP_ERR_TIMEOUT ? 3000 : 10000;
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }

            if (activated) {
                ESP_LOGI(TAG, "Device binding successful");
                Board::GetInstance().GetDisplay()->DismissActivation();
            } else {
                ESP_LOGW(TAG, "Device binding did not complete during startup");
            }
        }
    }

    const esp_err_t resource_check = ota_->CheckResources();
    if (resource_check != ESP_OK && resource_check != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "Resource OTA metadata check failed; postpone Xiaoling WebSocket connection");
        last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        return;
    }
    if (!CheckAssetsVersion()) {
        ESP_LOGE(TAG, "Resource OTA failed, postpone Xiaoling WebSocket connection");
        last_error_message_ = Lang::Strings::DOWNLOAD_ASSETS_FAILED;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        return;
    }

    InitializeXiaolingConn();

    // 通知主循环启动流程已完成。
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

bool Application::CheckAssetsVersion() {
    if (assets_version_checked_) {
        return true;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto& assets = Assets::GetInstance();
    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Resource partitions are unavailable for board %s", BOARD_NAME);
        return true;
    }

    Settings settings("assets", true);
    struct PendingResource {
        Assets::Resource resource;
        const char* key;
        const char* alias_key;
        const char* label;
        const char* updating_text;
        const Ota::ResourceInfo* remote;
    };
    const PendingResource pending[] = {
        {Assets::Resource::Respak, "respak_url", "download_url", "respak",
         Lang::Strings::UPDATING_RESPAK_RESOURCE, nullptr},
        {Assets::Resource::Tone, "tone_url", nullptr, "tone",
         Lang::Strings::UPDATING_TONE_RESOURCE, &ota_->GetToneResource()},
        {Assets::Resource::Emoji, "emoji_url", nullptr, "emoji",
         Lang::Strings::UPDATING_EMOJI_RESOURCE, &ota_->GetEmojiResource()},
        {Assets::Resource::WakeWord, "wake_word_url", "wakeword_url", "wake_word",
         Lang::Strings::UPDATING_WAKE_WORD_RESOURCE, &ota_->GetWakeWordResource()},
    };

    bool downloaded = false;
    bool failed = false;
    bool resource_tone_played = false;
    bool tone_downloaded = false;
    for (const auto& item : pending) {
        // 临时关闭唤醒词资源 OTA。KvOta::GetValue() 对未设置的唤醒词 KV
        // 返回禁用值；设置 user.disable_wakeword_update=0 可重新开启。
        if ((item.resource == Assets::Resource::WakeWord &&
             KvOta::IsDisabled(KvOta::UpdateType::WakeWord)) ||
            (item.resource == Assets::Resource::Tone &&
             KvOta::IsDisabled(KvOta::UpdateType::Tone)) ||
            (item.resource == Assets::Resource::Emoji &&
             KvOta::IsDisabled(KvOta::UpdateType::Emoji))) {
            ESP_LOGI(TAG, "Skipping %s OTA: disabled by user KV", item.label);
            continue;
        }
        std::string url = settings.GetString(item.key);
        if (url.empty() && item.alias_key != nullptr) {
            url = settings.GetString(item.alias_key);
        }
        size_t expected_size = 0;
        std::array<uint8_t, 16> expected_md5{};
        if (item.remote != nullptr && item.remote->available) {
            url = item.remote->url;
            expected_size = item.remote->size;
            expected_md5 = item.remote->md5;
            if (assets.VerifyResource(item.resource, expected_size, expected_md5)) {
                continue;
            }
        }
        if (url.empty()) {
            continue;
        }
        settings.EraseKey(item.key);
        if (item.alias_key != nullptr) settings.EraseKey(item.alias_key);

        ESP_LOGI(TAG, "Downloading %s resource from %s", item.label, url.c_str());
        SetDeviceState(kDeviceStateUpgrading);
        if (!resource_tone_played) {
            PlayOtaTone(89);
            resource_tone_played = true;
        }
        UiApp::ShowUpgradeProgress(item.updating_text, 0);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        const bool success = assets.DownloadResource(item.resource, url, expected_size, expected_md5,
            [label = std::string(item.updating_text)](int progress, size_t speed) {
                UiApp::ShowUpgradeProgress(label, progress, speed);
            });
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        if (!success) {
            ESP_LOGE(TAG, "Failed to update %s resource", item.label);
            failed = true;
            continue;
        }
        downloaded = true;
        if (item.resource == Assets::Resource::Tone) {
            tone_downloaded = true;
        }
    }

    if (failed && !downloaded) {
        PlayOtaTone(92);
        UiApp::ShowUpgradeResult(Lang::Strings::RESOURCE_UPDATE_FAILED);
        vTaskDelay(pdMS_TO_TICKS(3000));
        UiApp::NavigateHome();
        Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED,
              "circle_xmark", "");
        SetDeviceState(kDeviceStateActivating);
        return false;
    }
    if (downloaded) {
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
        if (tone_downloaded &&
            !ToneModel::GetInstance().Initialize(assets.ToneStore(), "/")) {
            ESP_LOGW(TAG, "Failed to reload tone model after tone update");
        }
#endif
        // Tone/emoji/wake-word models may still be referenced by running tasks;
        // reboot after the transaction so every consumer remaps a new image.
        if (failed) {
            PlayOtaTone(92);
            UiApp::ShowUpgradeResult(Lang::Strings::RESOURCE_UPDATE_FAILED);
        } else {
            PlayOtaTone(90);
            UiApp::ShowUpgradeResult(Lang::Strings::RESOURCE_UPDATE_SUCCESSFUL);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
        Reboot();
        return true;
    }

    assets.Apply();
    LocalizedAudio::GetInstance().Initialize(assets.RespakStore());
#ifdef CONFIG_USE_XIAOLING_MESSAGE_STYLE
    ToneModel::GetInstance().Initialize(assets.ToneStore(), "/");
    UiApp::ReloadEmojiResources();
#endif
    UiApp::SetChatMessage("system", "");
    UiApp::SetEmotion("microchip_ai");
    return true;
}

bool Application::CheckNewVersion() {
    UiApp::SetStatus(UiApp::Status::CheckingNewVersion);
    if (ota_ == nullptr || ota_->CheckVersion() != ESP_OK) {
        return false;
    }
    Settings ota_settings("ota", true);
    const bool app_update_succeeded = ota_settings.GetBool("app_update_pending_success", false);
    if (app_update_succeeded) {
        ota_settings.EraseKey("app_update_pending_success");
        PlayOtaTone(88);
    }
    if (!KvOta::IsDisabled(KvOta::UpdateType::App) && ota_->HasNewVersion()) {
        ESP_LOGI(TAG, "APP OTA available: %s, waiting for user confirmation", ota_->GetFirmwareVersion().c_str());

        const std::string description = ota_->GetFirmwareDescription().empty()
            ? Lang::Strings::OTA_DEFAULT_NOTES
            : ota_->GetFirmwareDescription();
        char confirmation[768] = {};
        if (ota_->GetFirmwareSize() != 0) {
            std::snprintf(confirmation, sizeof(confirmation), Lang::Strings::OTA_CONFIRM_WITH_SIZE,
                          ota_->GetFirmwareVersion().c_str(),
                          FormatFirmwareSize(ota_->GetFirmwareSize()).c_str(),
                          description.c_str());
        } else {
            std::snprintf(confirmation, sizeof(confirmation), Lang::Strings::OTA_CONFIRM,
                          ota_->GetFirmwareVersion().c_str(), description.c_str());
        }
        UiApp::SetStatusText(Lang::Strings::OTA_UPGRADE);
        UiApp::SetEmotion("download");
        UiApp::SetChatMessage("system", confirmation);

        ota_confirmation_decision_.store(0);
        ota_confirmation_pending_.store(true);
        PlayOtaTone(86, 3);
        const EventBits_t decision_bits = xEventGroupWaitBits(
            event_group_, MAIN_EVENT_OTA_CONFIRM_START | MAIN_EVENT_OTA_CONFIRM_SKIP,
            pdTRUE, pdFALSE, portMAX_DELAY);
        ota_confirmation_pending_.store(false);
        ota_confirmation_decision_.store(0);

        if ((decision_bits & MAIN_EVENT_OTA_CONFIRM_START) != 0) {
            ESP_LOGI(TAG, "User confirmed APP OTA: %s", ota_->GetFirmwareVersion().c_str());
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return false;
            }
            ESP_LOGW(TAG, "APP OTA failed; continuing with the current firmware");
        } else {
            ESP_LOGI(TAG, "User skipped APP OTA");
            UiApp::SetChatMessage("system", "");
        }
    }
    if (KvOta::IsDisabled(KvOta::UpdateType::App) && ota_->HasNewVersion()) {
        ESP_LOGI(TAG, "Skipping APP OTA: disabled by user KV");
    }
    ota_->MarkCurrentVersionValid();
    return true;
}
void Application::InitializeXiaolingConn() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    UiApp::SetStatus(UiApp::Status::LoadingProtocol);

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
        UiApp::SetDuplexVisible(true);
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
        UiApp::SetDuplexVisible(false);
        Schedule([this]() {
            if (GetDeviceState() == kDeviceStateWifiConfiguring) {
                return;
            }
            UiApp::SetChatMessage("system", "");
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
                HomePresenter::GetInstance().HandleSettingMessage(data);
#endif
                // Banner UI updates are queued first. The QR-code presenter
                // queues its download start afterwards so network/image work
                // cannot delay the first banner frame.
                QrCodePresenter::GetInstance().HandleSettingMessage(data);
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

    UiApp::SetEmotion("microchip_ai");
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
    const std::string activation_title = message.empty() ? Lang::Strings::ACTIVATION : message;
    const std::string activation_text = code.empty()
        ? activation_title
        : activation_title + "\n" + Lang::Strings::ACTIVATION_CODE + code;

#ifdef CONFIG_LSPLATFORM
    ESP_LOGI(TAG, "Showing activation code: %s, QR url: %s", code.c_str(), qrcode.c_str());
    if (!qrcode.empty()) {
        auto& board = Board::GetInstance();
        auto fetcher = board.GetImageFetcher();
        auto image = fetcher->Fetch(qrcode);
        if (!image) {
            ESP_LOGE(TAG, "Failed to fetch QR code image");
            return;
        }
        QrCodePresenter::GetInstance().SetCustomQrCode(std::move(image), activation_text);
        UiManager::GetInstance().NavigateTo(UI_PAGE_QRCODE);
    } else {
        ESP_LOGW(TAG, "QR code URL is empty, skipping image download");
    }

#else // !CONFIG_LSPLATFORM
    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, activation_text.c_str(), "link", "");
#endif // CONFIG_LSPLATFORM
    PlayActivationAudio(LocalizedAudio::kActivationGuideId);
    for (const auto& digit : code) {
        if (digit >= '0' && digit <= '9') {
            PlayActivationAudio(static_cast<uint16_t>(
                LocalizedAudio::kActivationDigitFirstId + digit - '0'));
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    UiApp::SetStatusText(status);
    UiApp::SetEmotion(emotion);
    UiApp::SetChatMessage("system", message);
    if (!sound.empty()) {
        PlayLocalizedAudio(std::string(sound).c_str());
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, uint16_t tone_id) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    UiApp::SetStatusText(status);
    UiApp::SetEmotion(emotion);
    UiApp::SetChatMessage("system", message);
    PlayTone(tone_id);
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        UiApp::SetStatus(UiApp::Status::Standby);
        UiApp::SetEmotion("neutral");
        UiApp::SetChatMessage("system", "");
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
    if (exit_requested_.load() &&
        (tts_waiting_for_playback_.load() || audio_player_.IsPlaying())) {
        ESP_LOGI(TAG, "忽略退出期间排队的唤醒词，TTS 尚未播放完成");
        audio_service_.EnableWakeWordDetection(false);
        return;
    }
    PauseWakeWordDetection();

    // Only accept audio wake words while the device is standing by. Wake-word
    // candidates during a conversation, playback, or activation are ignored.
    auto state = GetDeviceState();
    if (state != kDeviceStateIdle) {
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
    UiManager::GetInstance().Dispatch([icon = std::move(icon)]() {
        HomePresenter::GetInstance().SetWifiIcon(icon);
    });
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    if (reason == kAbortReasonExit) {
        exit_requested_.store(true);
        // The exit MCP tool ends the current uplink conversation only. TTS
        // may already be downloading or playing, so leave the player alone.
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

    // In half-duplex mode the exit tool can arrive while the microphone
    // session is still listening.  A speaking cancel only stops playback and
    // does not close that cloud session, so explicitly send the session end.
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
    exit_requested_.store(false);
    if (!SetDeviceState(kDeviceStateConnecting)) {
        return;
    }
    iat_utterance_active_ = false;
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
        tts_waiting_for_playback_.store(true);
        tts_output_started_.store(false);
        if (exit_requested_.load()) {
            // The MCP exit can race the URL notification. Keep the detector
            // disabled even when the URL arrives after the exit request.
            audio_service_.EnableWakeWordDetection(false);
        }
        Schedule([this]() {
            // URL arrival precedes the first decoded PCM by several seconds
            // for streamed TTS. Show a waiting emotion until playback really
            // starts; the kPlaying event restores the speaking emotion.
            if (GetDeviceState() == kDeviceStateListening ||
                GetDeviceState() == kDeviceStateSpeaking) {
                UiApp::SetStatusText("思考中...");
                UiApp::SetEmotion("等待");
            }
            if (session_controller_.policy().mode == DuplexMode::kHalfDuplex &&
                GetDeviceState() == kDeviceStateListening) {
                ESP_LOGI(TAG, "半双工收到 TTS URL，结束当前上行会话");
                audio_service_.EnableVoiceProcessing(false);
                if (xiaoling_conn_) xiaoling_conn_->SendStopListening();
            }
        });
    });
    if (iat_subscription_id_ == 0) {
        auto on_iat_text = [this](const XiaolingEvent& event) {
            Schedule([this, text = event.text]() {
                if (text.empty() || iat_utterance_active_) {
                    return;
                }
                iat_utterance_active_ = true;
                if (GetDeviceState() == kDeviceStateListening) {
                    // A valid IAT result means the user has spoken. Start a
                    // fresh idle window so the timeout measures silence after
                    // the utterance rather than time since wakeup.
                    listening_started_at_us_ = esp_timer_get_time();
                    ESP_LOGI(TAG, "收到 IAT，重置录音空闲计时");
                }
                if (session_controller_.policy().allow_barge_in &&
                    !audio_player_.IsWakeupPending() &&
                    GetDeviceState() == kDeviceStateSpeaking && audio_player_.IsPlaying()) {
                    // Keep the cloud session and stop only local TTS on real speech.
                    audio_player_.StopNonMusic(PlaybackEndReason::kPreempted);
                }
            });
        };
        iat_subscription_id_ = XiaolingEventBus::GetInstance().Subscribe(
            XIAOLING_IAT_START, on_iat_text);
        iat_update_subscription_id_ = XiaolingEventBus::GetInstance().Subscribe(
            XIAOLING_IAT_UPDATE, on_iat_text);
        iat_end_subscription_id_ = XiaolingEventBus::GetInstance().Subscribe(
            XIAOLING_IAT_END, [this](const XiaolingEvent&) {
                Schedule([this]() { iat_utterance_active_ = false; });
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
    if (new_state != kDeviceStateIdle) {
        keep_voice_processing_on_idle_ = false;
    }
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
            listening_started_at_us_ = 0;
            session_controller_.EndSession();
            if (tts_waiting_for_playback_.load() && !exit_requested_.load()) {
                UiApp::SetStatusText("思考中...");
                UiApp::SetEmotion("等待");
            } else {
                UiApp::SetStatus(UiApp::Status::Standby);
                UiApp::SetEmotion("待机");
            }
#ifdef CONFIG_LSPLATFORM
            UiApp::DismissOverlays();
#endif // CONFIG_LSPLATFORM
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
#endif // CONFIG_LSPLATFORM
            break;
        case kDeviceStateConnecting:
            // 唤醒提示音阶段的过渡态：连接已就绪，直接预告聆听，避免
            // "连接中..." 一闪而过。
            UiApp::SetStatus(UiApp::Status::Listening);
            UiApp::SetEmotion("聆听");
            UiApp::SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            listening_started_at_us_ = esp_timer_get_time();
            session_controller_.StartListening(listening_mode_ == kListeningModeRealtime);
            UiApp::SetStatus(UiApp::Status::Listening);
            UiApp::SetEmotion("聆听");
            UiApp::SetVoicePrompt(Lang::Strings::PLEASE_SPEAK);
#ifdef CONFIG_LSPLATFORM
            UiApp::DismissOverlays();
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
            UiApp::SetStatus(UiApp::Status::Speaking);
            UiApp::SetEmotion("说话");

            if (listening_mode_ != kListeningModeRealtime ||
                session_controller_.policy().mode == DuplexMode::kSimplex) {
                audio_service_.EnableVoiceProcessing(false);
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Stop();
#endif // CONFIG_LSPLATFORM
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

    UiApp::SetStatusText(Lang::Strings::OTA_UPGRADE);
    UiApp::SetEmotion("download");
    UiApp::SetChatMessage("system", Lang::Strings::UPGRADING);
    // The confirmation tone has just been cancelled by the button handler;
    // start directly with the "updating" prompt instead of replaying 086.
    PlayOtaTone(87);

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    UiApp::SetChatMessage("system", message);
    UiApp::ShowUpgradeProgress("APP", 0);

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [](int progress, size_t speed) {
        UiApp::ShowUpgradeProgress("APP", progress, speed);
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        PlayOtaTone(92);
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", "");
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        Settings ota_settings("ota", true);
        ota_settings.SetBool("app_update_pending_success", true);
        UiApp::SetChatMessage("system", Lang::Strings::UPGRADE_SUCCESSFUL_REBOOTING);
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
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
            UiApp::ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            UiApp::ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            session_controller_.SetMode(DuplexMode::kFullDuplex);
            UiApp::ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }
        if (aec_mode_ != kAecOnDeviceSide) {
            session_controller_.SetMode(DuplexMode::kHalfDuplex);
        }
        listening_mode_ = session_controller_.policy().mode == DuplexMode::kFullDuplex
                              ? kListeningModeRealtime
                              : kListeningModeAutoStop;
        UiApp::SetDuplexMode(session_controller_.policy().mode);

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
        if (mode == session_controller_.policy().mode) {
            return;
        }

        const auto state = GetDeviceState();
        const bool voice_processing_running = audio_service_.IsAudioProcessorRunning();

        // End the current cloud session and stop only the uplink. Keeping the
        // processor alive avoids tearing down AFE while returning to wake-word detection.
        iat_utterance_active_ = false;
        audio_service_.EnableAudioUpload(false);
        session_controller_.EndSession();
        if (xiaoling_conn_ && !xiaoling_conn_->EndSession()) {
            ESP_LOGW(TAG, "切换交互模式时上报会话结束失败");
        }

        session_controller_.SetMode(mode);
        UiApp::SetDuplexMode(mode);
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

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlayLocalizedAudio(const char* name) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (name == nullptr || !LocalizedAudio::GetInstance().GetData(name, &data, &size)) {
        ESP_LOGW(TAG, "本地化提示音不存在: %s", name != nullptr ? name : "(null)");
        return;
    }
    audio_service_.PlaySound(std::string_view(reinterpret_cast<const char*>(data), size));
}

void Application::PlayActivationAudio(uint16_t id) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!LocalizedAudio::GetInstance().GetActivationData(id, &data, &size)) {
        ESP_LOGW(TAG, "激活音频不存在: id=%u", static_cast<unsigned>(id));
        return;
    }
    audio_service_.PlaySound(std::string_view(reinterpret_cast<const char*>(data), size));
}

void Application::PlayTone(uint16_t tone_id) {
    if (audio_player_.PlayTone(tone_id) == 0) {
        ESP_LOGW(TAG, "ROMFS tone 播放失败: id=%u", static_cast<unsigned>(tone_id));
    }
}

void Application::PlayOtaTone(uint16_t tone_id, uint8_t repeat_count) {
    for (uint8_t repeat = 0; repeat < repeat_count; ++repeat) {
        if (ota_confirmation_pending_.load() && ota_confirmation_decision_.load() != 0) {
            return;
        }
        if (audio_player_.PlayTone(tone_id) == 0) {
            ESP_LOGW(TAG, "OTA tone playback failed: id=%u", static_cast<unsigned>(tone_id));
            return;
        }

        const TickType_t start = xTaskGetTickCount();
        bool started = false;
        while (xTaskGetTickCount() - start < pdMS_TO_TICKS(15000)) {
            if (ota_confirmation_pending_.load() && ota_confirmation_decision_.load() != 0) {
                audio_player_.Stop(PlaybackEndReason::kCancelled);
                return;
            }
            if (audio_player_.IsPlaying()) {
                started = true;
            } else if (started) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
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
