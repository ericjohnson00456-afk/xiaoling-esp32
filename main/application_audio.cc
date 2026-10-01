#include "application.h"

#include <esp_log.h>

#include "board.h"
#include "event/app_event.h"
#include "xiaoling/xiaoling_events.h"
#include "ui/ui.h"

#define TAG "ApplicationAudio"

void Application::InitializeAudio() {
    // 音频硬件初始化
    auto codec = Board::GetInstance().GetAudioCodec();
    // 音频服务初始化
    audio_service_.Initialize(codec);
    Ui::SetWakeWord(audio_service_.GetWakeWordName());

    // 播放器初始化
    if (!audio_player_.Initialize(&audio_service_)) {
        ESP_LOGE(TAG, "音频播放器初始化失败");
    }
    // 播放器事件栈初始化
    intent_manager_.Initialize(*this);

    // 生产者启动前先装好所有订阅者和发布回调，避免遗漏启动阶段的事件。
    RegisterAudioEventSubscribers();
    BindAudioEventPublishers();

    audio_service_.Start();
    audio_service_.EnableWakeWordDetection(true);
}

void Application::RegisterAudioEventSubscribers() {
    // Application::Initialize() 只应调用一次；此保护同时避免意外重复订阅。
    if (!playback_event_subscription_ids_.empty() ||
        !playback_output_event_subscription_ids_.empty() ||
        !audio_event_subscription_ids_.empty()) {
        ESP_LOGW(TAG, "音频事件已订阅，跳过重复注册");
        return;
    }

    // 播放器先在 app_event 中整理内部状态，再发布统一的公共生命周期事件。
    const auto playback_handler = [](const AppEvent::Event& queued, void* context) {
        const auto* event = queued.Get<PlaybackEvent>();
        if (event == nullptr) {
            ESP_LOGE(TAG, "播放生命周期事件数据无效");
            return;
        }
        static_cast<Application*>(context)->HandlePlaybackEvent(*event);
    };
    constexpr AppEvent::Id kPlaybackEvents[] = {
        AppEvent::Id::PlaybackStreamReady,
        AppEvent::Id::PlaybackStarted,
        AppEvent::Id::PlaybackPaused,
        AppEvent::Id::PlaybackStopped,
        AppEvent::Id::PlaybackCompleted,
        AppEvent::Id::PlaybackFailed,
    };
    for (const auto id : kPlaybackEvents) {
        const auto subscription_id = AppEvent::Subscribe(id, playback_handler, this);
        if (subscription_id == 0) {
            ESP_LOGE(TAG, "订阅播放生命周期事件失败: event=%u",
                     static_cast<unsigned>(id));
        } else {
            playback_event_subscription_ids_.push_back(subscription_id);
        }
    }

    const struct {
        AppEvent::Id id;
        AppEvent::Handler handler;
    } audio_events[] = {
        {AppEvent::Id::WakeWordDetected,
         [](const AppEvent::Event&, void* context) {
             static_cast<Application*>(context)->HandleWakeWordDetectedEvent();
         }},
        {AppEvent::Id::VadChanged,
         [](const AppEvent::Event&, void* context) {
             static_cast<Application*>(context)->HandleVadChangedEvent();
         }},
    };
    for (const auto& registration : audio_events) {
        const auto subscription_id = AppEvent::Subscribe(
            registration.id, registration.handler, this);
        if (subscription_id == 0) {
            ESP_LOGE(TAG, "订阅音频输入事件失败: event=%u",
                     static_cast<unsigned>(registration.id));
        } else {
            audio_event_subscription_ids_.push_back(subscription_id);
        }
    }

    const struct {
        AppEvent::Id id;
        AppEvent::Handler handler;
    } playback_output_events[] = {
        {AppEvent::Id::PlayerOutputStarted,
         [](const AppEvent::Event& queued, void* context) {
             const auto* event = queued.Get<PlaybackOutputEvent>();
             if (event == nullptr) {
                 ESP_LOGE(TAG, "播放器首帧事件数据无效");
                 return;
             }
             static_cast<Application*>(context)->HandlePlayerOutputStartedEvent(*event);
         }},
        {AppEvent::Id::PlayerOutputProgress,
         [](const AppEvent::Event&, void* context) {
             auto* application = static_cast<Application*>(context);
             application->HandlePlayerOutputProgressEvent(
                 application->audio_player_.TakeLatestOutputProgress());
         }},
    };
    for (const auto& registration : playback_output_events) {
        const auto subscription_id = AppEvent::Subscribe(
            registration.id, registration.handler, this);
        if (subscription_id == 0) {
            ESP_LOGE(TAG, "订阅播放器输出事件失败: event=%u",
                     static_cast<unsigned>(registration.id));
        } else {
            playback_output_event_subscription_ids_.push_back(subscription_id);
        }
    }
}

void Application::BindAudioEventPublishers() {
    AudioServiceCallbacks callbacks;
    callbacks.on_wake_word_detected = [](const std::string&) {
        if (!AppEvent::Publish(AppEvent::Id::WakeWordDetected, nullptr, 0,
                               AppEvent::Priority::Urgent)) {
            ESP_LOGE(TAG, "发布唤醒词事件失败");
        }
    };
    callbacks.on_vad_change = [this](bool speaking) {
        (void)speaking;
        if (!vad_event_pending_.exchange(true) &&
            !AppEvent::Publish(AppEvent::Id::VadChanged)) {
            vad_event_pending_.store(false);
        }
    };
    audio_service_.SetCallbacks(callbacks);

    AudioServiceUploadCallbacks upload_callbacks;
    upload_callbacks.on_opus_packet_available = [this]() {
        // Opus 包属于实时数据，只直接交给网络发送队列，不经过应用事件循环。
        auto* connection = xiaoling_conn_.get();
        if (connection == nullptr) {
            return;
        }
        if (!connection->IsSessionActive()) {
            // 云端会话失效后关闭网络消费端；本地采集、AFE 和 Opus 编码仍常驻。
            ESP_LOGW(TAG, "云端语音会话已失效，暂停本地音频上行");
            audio_service_.EnableAudioUpload(false);
            return;
        }
        constexpr size_t kMaxDirectHandoffPackets = 8;
        for (size_t i = 0; i < kMaxDirectHandoffPackets; ++i) {
            auto packet = audio_service_.PopOpusPacket();
            if (packet == nullptr) {
                break;
            }
            connection->SendAudio(std::move(packet));
        }
    };
    audio_service_.SetUploadCallbacks(upload_callbacks);

    // TTS URL 回调属于固定接线，只在初始化时设置一次，不随每次唤醒重复设置。
    audio_player_.SetTtsUrlCallback([this]() {
        const uint32_t generation = tts_url_generation_.fetch_add(1) + 1;
        tts_waiting_for_playback_.store(true);
        tts_output_started_.store(false);
        ESP_LOGI(TAG, "TTS URL 已接纳: generation=%lu",
                 static_cast<unsigned long>(generation));
        if (exit_requested_.load()) {
            // MCP exit 可能与 URL 通知竞态；退出请求后到达的 URL 也不能重开唤醒检测。
            audio_service_.EnableWakeWordDetection(false);
        }
        Schedule([this]() {
            const bool has_music =
                intent_manager_.Contains(PlaybackIntentId::kMusic);
            const bool was_listening = GetDeviceState() == kDeviceStateListening;
            // URL 到达后先更新等待文案，真正出声后再切换到讲话状态。
            if (was_listening || GetDeviceState() == kDeviceStateSpeaking) {
                Ui::SetStatusText("思考中...");
                Ui::SetEmotion("等待");
            }
            if (session_controller_.policy().mode == DuplexMode::kHalfDuplex &&
                was_listening) {
                ESP_LOGI(TAG, "半双工收到 TTS URL，结束当前上行会话");
                audio_service_.EnableAudioUpload(false);
                if (xiaoling_conn_) {
                    xiaoling_conn_->SendStopListening();
                }
            }
            if (has_music) {
                ESP_LOGI(TAG,
                         "音乐事件存在，收到 TTS URL 后仅停止本地录音，保持云端合成");
                audio_service_.EnableAudioUpload(false);
#ifdef CONFIG_LSPLATFORM
                uplink_watchdog_.Stop();
#endif  // CONFIG_LSPLATFORM
                // 不发送 action:cancel；TTS 结束后的统一收尾会结束云端会话。
            }
        });
    });
}
