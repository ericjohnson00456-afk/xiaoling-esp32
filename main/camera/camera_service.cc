#include "camera_service.h"

#include "application.h"
#include "boards/common/board.h"
#include "boards/common/camera.h"
#include "player/intents/camera_capture_intent.h"
#include "system/device_status.h"
#include "system/psram_task.h"
#include "ui/ui.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>

#include <array>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

constexpr char kTag[] = "CameraService";
constexpr uint16_t kCaptureToneId = 73;
constexpr uint16_t kPreviewWidth = 160;
constexpr uint16_t kPreviewHeight = 120;
constexpr TickType_t kPreviewWaitInterval = pdMS_TO_TICKS(20);
constexpr TickType_t kCaptureToneWaitTimeout = pdMS_TO_TICKS(2000);
constexpr TickType_t kTtsStartWaitTimeout = pdMS_TO_TICKS(12000);
constexpr uint32_t kVoiceAutoCaptureDelayMs = 3000;
constexpr configSTACK_DEPTH_TYPE kWorkerStackSize = 12 * 1024;
constexpr UBaseType_t kWorkerPriority = 5;
constexpr int64_t kStartupGuardUs = 3 * 1000 * 1000;
constexpr int64_t kEarlyVoiceTtsWindowUs = 10 * 1000 * 1000;
constexpr char kVisionPrompt[] =
    "这个图片里面有什么东西";

}  // 匿名命名空间

CameraService& CameraService::GetInstance() {
    static CameraService service;
    return service;
}

CameraService::~CameraService() {
    for (const auto subscription : subscriptions_) {
        AppEvent::Unsubscribe(subscription);
    }
}

void CameraService::Initialize(Application& application) {
    if (application_ != nullptr) {
        return;
    }
    application_ = &application;
    application.GetIntentManager().CameraCapture().SetCancelCallback(
        [this]() { HandleIntentPreempted(); });
    const std::array<AppEvent::Id, 6> playback_events = {
        AppEvent::Id::PlaybackStreamReady,
        AppEvent::Id::PlaybackStarted,
        AppEvent::Id::PlaybackPaused,
        AppEvent::Id::PlaybackCompleted,
        AppEvent::Id::PlaybackStopped,
        AppEvent::Id::PlaybackFailed,
    };
    for (const auto id : playback_events) {
        const auto subscription =
            AppEvent::Subscribe(id, [this](const AppEvent::Event& queued) {
                const auto* event = queued.Get<PlaybackEvent>();
                if (event != nullptr) {
                    HandlePlaybackEvent(*event);
                }
            });
        if (subscription != 0) {
            subscriptions_.push_back(subscription);
        } else {
            ESP_LOGE(kTag, "订阅播放器事件失败: event=%u",
                     static_cast<unsigned>(id));
        }
    }
}

bool CameraService::IsActive() const {
    return phase_.load() != Phase::Inactive;
}

bool CameraService::StartButtonCapture() {
    return Start(Origin::Button, 0);
}

bool CameraService::StartVoiceMcpCapture(bool sync,
                                         VoiceMcpResultCallback callback) {
    if (!callback) {
        return false;
    }
    if (phase_.load() != Phase::Inactive) {
        callback(false, "当前已有拍照任务正在进行");
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    const bool tts_waiting_without_pcm =
        application_ != nullptr &&
        application_->GetDeviceState() == kDeviceStateListening &&
        application_->GetAudioPlayer().IsPlaying();
    bool adopted_early_tts = false;
    std::string adopted_url;

    // 从 MCP 工具开始执行就关闭 TTS 放行门。若 TTS URL 比工具调用更早
    // 到达，则接管正在空等云端合成的 URL，待工具响应后重新提交播放器。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voice_tts_gate_active_ = true;
        voice_mcp_sync_ = sync;
        voice_mcp_result_sent_ = false;
        deferred_voice_tts_url_.clear();
        deferred_voice_tts_pushup_ = false;
        if (!sync && tts_waiting_without_pcm && !recent_tts_url_.empty() &&
            now_us >= recent_tts_url_at_us_ &&
            now_us - recent_tts_url_at_us_ <= kEarlyVoiceTtsWindowUs) {
            adopted_url = recent_tts_url_;
            deferred_voice_tts_url_ = adopted_url;
            deferred_voice_tts_pushup_ = recent_tts_pushup_;
            adopted_early_tts = true;
        }
    }
    ESP_LOGI(kTag, "MCP 拍照模式: sync=%d, 等待主动 TTS=%d",
             sync ? 1 : 0, sync ? 0 : 1);

    if (adopted_early_tts && application_ != nullptr) {
        // 必须在拍照 Intent 停止旧播放器之前标记重播，否则极快的 STOPPED
        // 事件可能先清除已经到达的 timeline。
        application_->PrepareTtsReplay();
    }

    std::string error;
    if (Start(Origin::VoiceMcp, kVoiceAutoCaptureDelayMs,
              callback, &error)) {
        if (adopted_early_tts) {
            ESP_LOGI(kTag, "已接管提前到达的拍照 TTS，等待 MCP 响应后重播");
        }
        return true;
    }

    if (adopted_early_tts) {
        // 相机未能启动时不重播仍在运行的原 TTS，只需在错误响应后解除门控。
        std::lock_guard<std::mutex> lock(mutex_);
        if (deferred_voice_tts_url_ == adopted_url) {
            deferred_voice_tts_url_.clear();
            deferred_voice_tts_pushup_ = false;
        }
    }
    callback(false, error.empty() ? "无法启动拍照" : std::move(error));
    if (application_ != nullptr) {
        application_->Schedule([this]() { ReleaseVoiceMcpTtsGate(false); });
    } else {
        ReleaseVoiceMcpTtsGate(false);
    }
    return false;
}

bool CameraService::CancelVoiceMcpCountdown(const char* reason) {
    if (phase_.load() != Phase::Preview ||
        origin_.load() != Origin::VoiceMcp) {
        return false;
    }
    const uint32_t generation = generation_.load();
    ESP_LOGI(kTag, "取消语音拍照倒计时: %s",
             reason != nullptr ? reason : "用户取消拍照");
    CompleteVoiceMcp(generation, false,
                     reason != nullptr ? reason : "用户取消拍照", false);
    return true;
}

bool CameraService::DeferVoiceMcpTtsUrl(const std::string& url, bool pushup) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (voice_tts_gate_active_ && voice_mcp_sync_ && pushup) {
        // sync:true 不期待 pushup TTS；不能让它覆盖正常回复或在工具完成后复播。
        ESP_LOGI(kTag, "sync 拍照期间忽略主动播报 TTS URL");
        return true;
    }
    if (voice_tts_gate_active_ && voice_mcp_sync_ &&
        !voice_mcp_result_sent_) {
        // 工具结果发出前到达的普通 TTS 也属于旧回复，不应在 sync 拍照后复播。
        ESP_LOGI(kTag, "sync 拍照结果发出前忽略旧 TTS URL");
        return true;
    }
    recent_tts_url_ = url;
    recent_tts_url_at_us_ = esp_timer_get_time();
    recent_tts_pushup_ = pushup;
    if (!voice_tts_gate_active_) {
        return false;
    }
    // 同一轮回复只应有一个语音 URL；若云端重复下发，以最新值为准。
    deferred_voice_tts_url_ = url;
    deferred_voice_tts_pushup_ = pushup;
    ESP_LOGI(kTag, "拍照 TTS URL 已暂存，等待 MCP 工具响应");
    return true;
}

bool CameraService::OwnsStandaloneTtsLifecycle() const {
    return IsActive() && origin_.load() == Origin::Button;
}

bool CameraService::Start(Origin origin,
                          uint32_t auto_capture_delay_ms,
                          VoiceMcpResultCallback callback,
                          std::string* error) {
    const auto set_error = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
    };
    auto* camera = Board::GetInstance().GetCamera();
    if (application_ == nullptr || camera == nullptr || !camera->IsAvailable()) {
        ESP_LOGW(kTag, "双击拍照失败：未检测到摄像头");
        Ui::ShowToast("未检测到摄像头");
        set_error("未检测到摄像头");
        return false;
    }
    if (esp_timer_get_time() < kStartupGuardUs) {
        ESP_LOGW(kTag, "启动保护期内忽略拍照预览请求");
        set_error("设备正在启动，请稍后重试");
        return false;
    }

    const DeviceState state = application_->GetDeviceState();
    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring ||
        state == kDeviceStateCheckingUpdate || state == kDeviceStateActivating ||
        state == kDeviceStateUpgrading || state == kDeviceStateFatalError) {
        ESP_LOGW(kTag, "当前状态不能进入拍照预览: %s",
                 DeviceStatus::GetStateName(state));
        Ui::ShowToast("当前状态暂时不能拍照");
        set_error("当前状态暂时不能拍照");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_.load() != Phase::Inactive || task_handle_ != nullptr) {
        set_error("当前已有拍照任务正在进行");
        return false;
    }

    // 先标记为活动态，再让 Intent 停止旧 TTS，避免旧播放器终态被应用层
    // 误判为普通语音会话结束并重新开启上行。
    phase_ = Phase::Starting;
    if (!application_->GetIntentManager().CameraCapture().Enter(
            origin == Origin::VoiceMcp)) {
        phase_ = Phase::Inactive;
        ESP_LOGW(kTag, "拍照事件无法取得播放优先级");
        Ui::ShowToast("当前状态暂时不能拍照");
        set_error("当前状态暂时不能拍照");
        return false;
    }
    camera_intent_active_ = true;

    capture_requested_ = false;
    exit_requested_ = false;
    preempted_exit_ = false;
    capture_tone_finished_ = false;
    capture_tone_session_id_ = 0;
    tts_session_id_ = 0;
    origin_ = origin;
    auto_capture_delay_ms_ = auto_capture_delay_ms;
    voice_mcp_result_callback_ = std::move(callback);
    const uint32_t generation = generation_.fetch_add(1) + 1;
    phase_ = Phase::Preview;
    if (origin == Origin::VoiceMcp) {
        Ui::ShowCameraPreview(Ui::CameraPreviewPhase::Countdown,
                              "3s 后拍照");
    } else {
        Ui::ShowCameraPreview();
    }

    auto context = std::make_unique<TaskContext>();
    context->service = this;
    context->generation = generation;
    if (PsramTask::Create(TaskEntry, "camera_service", kWorkerStackSize,
                          context.get(), kWorkerPriority, &task_handle_,
                          &context->stack_in_psram) != pdPASS) {
        task_handle_ = nullptr;
        phase_ = Phase::Inactive;
        Ui::DismissCameraPreview();
        ReleaseCameraIntent();
        voice_mcp_result_callback_ = {};
        set_error("创建相机预览任务失败");
        ESP_LOGE(kTag, "创建相机预览任务失败");
        return false;
    }
    context.release();
    ESP_LOGI(kTag, "进入%s拍照预览: generation=%lu",
             origin_.load() == Origin::Button ? "按键" : "语音/MCP",
             static_cast<unsigned long>(generation));
    return true;
}

bool CameraService::Capture() {
    if (phase_.load() != Phase::Preview) {
        return IsActive();
    }
    if (origin_.load() == Origin::VoiceMcp) {
        return false;
    }
    if (!capture_requested_.exchange(true)) {
        ESP_LOGI(kTag, "单击拍照请求已提交");
    }
    return true;
}

bool CameraService::Exit() {
    const Phase phase = phase_.load();
    if (phase == Phase::Inactive) {
        return false;
    }
    if (origin_.load() == Origin::VoiceMcp) {
        VoiceMcpResultCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = voice_mcp_result_callback_;
        }
        if (callback) {
            CompleteVoiceMcp(generation_.load(), false, "拍照已取消", false);
            return true;
        }
    }
    exit_requested_ = true;
    if (phase == Phase::Preview) {
        phase_ = Phase::Exiting;
        ESP_LOGI(kTag, "等待当前预览帧结束后退出");
    } else {
        FinishExit(!preempted_exit_.load());
    }
    return true;
}

void CameraService::TaskEntry(void* arg) {
    std::unique_ptr<TaskContext> context(static_cast<TaskContext*>(arg));
    auto* service = context->service;
    const uint32_t generation = context->generation;
    const bool stack_in_psram = context->stack_in_psram;
    service->TaskLoop(generation);
    {
        std::lock_guard<std::mutex> lock(service->mutex_);
        service->task_handle_ = nullptr;
    }
    context.reset();
    PsramTask::Delete(stack_in_psram);
}

void CameraService::TaskLoop(uint32_t generation) {
    auto* camera = Board::GetInstance().GetCamera();
    if (camera == nullptr || !camera->IsAvailable()) {
        Fail(generation, "未检测到摄像头");
        return;
    }

    bool preview_frame_ready = false;
    const TickType_t preview_started_at = xTaskGetTickCount();
    uint32_t displayed_countdown_seconds =
        (auto_capture_delay_ms_ + 999U) / 1000U;
    while (IsCurrent(generation)) {
        if (phase_.load() == Phase::Exiting || exit_requested_.load()) {
            FinishExit(!preempted_exit_.load());
            return;
        }
        if (phase_.load() != Phase::Preview) {
            return;
        }
        if (auto_capture_delay_ms_ > 0) {
            const TickType_t elapsed_ticks =
                xTaskGetTickCount() - preview_started_at;
            const uint32_t elapsed_ms =
                static_cast<uint32_t>(elapsed_ticks * portTICK_PERIOD_MS);
            if (elapsed_ms >= auto_capture_delay_ms_) {
                capture_requested_ = true;
                auto_capture_delay_ms_ = 0;
            } else {
                const uint32_t remaining_seconds =
                    (auto_capture_delay_ms_ - elapsed_ms + 999U) / 1000U;
                if (remaining_seconds != displayed_countdown_seconds) {
                    displayed_countdown_seconds = remaining_seconds;
                    Ui::SetCameraPreviewState(
                        Ui::CameraPreviewPhase::Countdown,
                        std::to_string(remaining_seconds) + "s 后拍照");
                }
            }
        }
        if (preview_frame_ready && capture_requested_.exchange(false)) {
            phase_ = Phase::Processing;
            Ui::FreezeCameraPreviewToHome("上传图片中...");
            capture_tone_finished_ = false;
            const uint32_t tone_session =
                application_->GetAudioPlayer().PlayTone(kCaptureToneId);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                capture_tone_session_id_ = tone_session;
            }
            if (tone_session == 0) {
                ESP_LOGW(kTag, "拍照提示音播放失败: tone=%u",
                         static_cast<unsigned>(kCaptureToneId));
            } else {
                // 等拍照提示音完全送入扬声器后再提交图片，避免云端快速返回的
                // TTS URL 抢占提示音，造成提示音不完整。
                ulTaskNotifyTake(pdTRUE, kCaptureToneWaitTimeout);
                if (!capture_tone_finished_.load()) {
                    ESP_LOGW(kTag,
                             "拍照提示音等待超时，停止残留音频后继续上传: session=%lu",
                             static_cast<unsigned long>(tone_session));
                    application_->GetAudioPlayer().StopNonMusic(
                        PlaybackEndReason::kPreempted);
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                capture_tone_session_id_ = 0;
            }
            if (!IsCurrent(generation) || exit_requested_.load()) {
                return;
            }
            if (origin_.load() == Origin::VoiceMcp) {
                UploadMcpPhoto(generation, *camera);
            } else {
                RequestCloudDescription(generation, *camera);
            }
            if (!IsCurrent(generation) || phase_.load() != Phase::WaitingTts) {
                return;
            }

            // 按键识图可用冻结帧重试一次；MCP 拍照已经把 URL 回给云端，
            // 只能等待该轮 TTS，不能重复上传并响应同一个工具调用。
            ulTaskNotifyTake(pdTRUE, kTtsStartWaitTimeout);
            if (origin_.load() == Origin::Button && IsCurrent(generation) &&
                phase_.load() == Phase::WaitingTts) {
                ESP_LOGW(kTag, "识图 TTS 等待超时，重试一次");
                RequestCloudDescription(generation, *camera);
                if (!IsCurrent(generation) || phase_.load() != Phase::WaitingTts) {
                    return;
                }
                ulTaskNotifyTake(pdTRUE, kTtsStartWaitTimeout);
            }
            if (IsCurrent(generation) && phase_.load() == Phase::WaitingTts) {
                Fail(generation, "识图语音响应超时");
            }
            return;
        }
        if (!camera->CaptureWithPreviewSize(kPreviewWidth, kPreviewHeight)) {
            Fail(generation, "未检测到摄像头");
            return;
        }
        preview_frame_ready = true;
        vTaskDelay(kPreviewWaitInterval);
    }
}

void CameraService::RequestCloudDescription(uint32_t generation,
                                            Camera& camera) {
    if (!IsCurrent(generation) || exit_requested_.load()) {
        FinishExit(true);
        return;
    }

    const std::string image_base64 = camera.EncodeJpegBase64(80);
    if (image_base64.empty()) {
        Fail(generation, "照片编码失败");
        return;
    }
    if (!application_->RequestImageRecognition(image_base64, kVisionPrompt)) {
        Fail(generation, "识图请求失败，请检查云端连接");
        return;
    }
    phase_ = Phase::WaitingTts;
    ESP_LOGI(kTag, "按键照片 Base64 已通过 WS 上报，等待识图 TTS");
}

void CameraService::UploadMcpPhoto(uint32_t generation, Camera& camera) {
    if (!IsCurrent(generation) || exit_requested_.load()) {
        CompleteVoiceMcp(generation, false, "拍照已取消", false);
        return;
    }

    try {
        // vision capability 下发的 HTTP 地址采用 multipart JPEG。Camera
        // 内部负责把当前完整帧编码后上传，返回 JSON 中的图片 URL。
        const std::string response = camera.Explain("");
        cJSON* json = cJSON_ParseWithLength(response.data(), response.size());
        if (json == nullptr) {
            throw std::runtime_error("照片上传响应格式错误");
        }
        const auto* data = cJSON_GetObjectItemCaseSensitive(json, "data");
        const auto* url = cJSON_IsObject(data)
                              ? cJSON_GetObjectItemCaseSensitive(data, "url")
                              : nullptr;
        std::string image_url;
        if (cJSON_IsString(url) && url->valuestring != nullptr) {
            image_url = url->valuestring;
        }
        cJSON_Delete(json);
        if (image_url.empty()) {
            throw std::runtime_error("照片上传响应缺少图片 URL");
        }

        phase_ = Phase::WaitingTts;
        ESP_LOGI(kTag, "MCP 照片已通过 HTTP 上传，准备响应图片 URL");
        bool sync = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sync = voice_mcp_sync_;
        }
        ESP_LOGI(kTag, "MCP 照片 URL 已就绪: sync=%d, 等待 TTS=%d",
                 sync ? 1 : 0, sync ? 0 : 1);
        CompleteVoiceMcp(generation, true, std::move(image_url), !sync);
    } catch (const std::exception& error) {
        CompleteVoiceMcp(generation, false, error.what(), false);
    }
}

void CameraService::CompleteVoiceMcp(uint32_t generation, bool success,
                                     std::string value,
                                     bool keep_until_tts) {
    VoiceMcpResultCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrent(generation) || origin_.load() != Origin::VoiceMcp) {
            return;
        }
        callback = std::move(voice_mcp_result_callback_);
    }
    if (!callback) {
        if (!keep_until_tts) {
            FinishExit(false);
        }
        return;
    }

    // 回调先把 MCP 响应加入 Application 队列；随后排入 Intent 释放和
    // TTS 放行任务，从队列顺序上保证响应先于音频拉流。
    callback(success, std::move(value));
    if (success) {
        std::lock_guard<std::mutex> lock(mutex_);
        voice_mcp_result_sent_ = true;
    }
    if (keep_until_tts) {
        ReleaseCameraIntent();
    } else {
        FinishExit(false);
    }
    if (application_ != nullptr) {
        application_->Schedule([this, success]() { ReleaseVoiceMcpTtsGate(success); });
    } else {
        ReleaseVoiceMcpTtsGate(success);
    }
}

void CameraService::ReleaseVoiceMcpTtsGate(bool replay) {
    std::string url;
    bool pushup = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!voice_tts_gate_active_) {
            return;
        }
        voice_tts_gate_active_ = false;
        url = std::move(deferred_voice_tts_url_);
        pushup = deferred_voice_tts_pushup_;
        deferred_voice_tts_url_.clear();
        deferred_voice_tts_pushup_ = false;
        recent_tts_url_.clear();
        recent_tts_url_at_us_ = 0;
        recent_tts_pushup_ = false;
        voice_mcp_sync_ = false;
        voice_mcp_result_sent_ = false;
    }
    if (replay && !url.empty() && application_ != nullptr) {
        ESP_LOGI(kTag, "MCP 工具响应已发送，放行拍照 TTS URL");
        // 保留原来的 pushup/普通 TTS 类型，避免主动播报误进入对话收尾。
        const auto event_id = pushup ? AppEvent::Id::XiaolingPushTtsUrl
                                     : AppEvent::Id::XiaolingTtsUrl;
        if (!AppEvent::PublishText(event_id, url)) {
            ESP_LOGW(kTag, "拍照 TTS URL 放行事件提交失败");
        }
    }
}

void CameraService::ReleaseCameraIntent() {
    if (application_ != nullptr && camera_intent_active_.exchange(false)) {
        application_->GetIntentManager().CameraCapture().Exit();
    }
}

void CameraService::HandlePlaybackEvent(const PlaybackEvent& event) {
    if (!IsActive()) {
        return;
    }

    if (event.channel == PlaybackChannel::kPrompt) {
        if (event.state != PlaybackState::kCompleted &&
            event.state != PlaybackState::kStopped &&
            event.state != PlaybackState::kFailed) {
            return;
        }
        TaskHandle_t task = nullptr;
        bool matched = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            matched = capture_tone_session_id_ != 0 &&
                      event.session_id == capture_tone_session_id_;
            task = task_handle_;
        }
        if (matched) {
            capture_tone_finished_ = true;
            ESP_LOGI(kTag, "拍照提示音已结束: session=%lu, state=%u",
                     static_cast<unsigned long>(event.session_id),
                     static_cast<unsigned>(event.state));
            if (task != nullptr) {
                xTaskNotifyGive(task);
            }
        }
        return;
    }

    if (event.channel != PlaybackChannel::kTts) {
        return;
    }

    const Phase phase = phase_.load();
    if (phase != Phase::Processing && phase != Phase::WaitingTts &&
        phase != Phase::Speaking) {
        return;
    }

    if (event.state == PlaybackState::kStreamReady ||
        event.state == PlaybackState::kPlaying ||
        event.state == PlaybackState::kPaused) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (tts_session_id_ == 0) {
                tts_session_id_ = event.session_id;
            }
        }
        phase_ = Phase::Speaking;
        Ui::SetStatusText("说话中");
        TaskHandle_t task = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task = task_handle_;
        }
        if (task != nullptr) {
            xTaskNotifyGive(task);
        }
        ESP_LOGI(kTag, "收到照片识别 TTS 播放事件: session=%lu, state=%u",
                 static_cast<unsigned long>(event.session_id),
                 static_cast<unsigned>(event.state));
        return;
    }

    if (event.state != PlaybackState::kCompleted &&
        event.state != PlaybackState::kStopped &&
        event.state != PlaybackState::kFailed) {
        return;
    }

    uint32_t tts_session = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tts_session = tts_session_id_;
    }
    if (tts_session != 0 && event.session_id == tts_session) {
        ESP_LOGI(kTag, "照片识别 TTS 已结束: session=%lu, state=%u",
                 static_cast<unsigned long>(event.session_id),
                 static_cast<unsigned>(event.state));
        application_->Schedule([this]() { FinishExit(false); });
    }
}

void CameraService::HandleIntentPreempted() {
    // 更高优先级事件已经接管播放器，退出拍照时不能再停止非音乐通道，
    // 否则会误停刚开始播放的闹钟等音频。
    preempted_exit_ = true;
    Exit();
}

void CameraService::Fail(uint32_t generation, const char* message) {
    if (!IsCurrent(generation)) {
        return;
    }
    if (origin_.load() == Origin::VoiceMcp) {
        ESP_LOGE(kTag, "%s", message);
        Ui::ShowToast(message, 3000);
        CompleteVoiceMcp(generation, false, message, false);
        return;
    }
    phase_ = Phase::Processing;
    ESP_LOGE(kTag, "%s", message);
    Ui::ShowToast(message, 3000);
    FinishExit(true);
}

void CameraService::FinishExit(bool stop_audio) {
    TaskHandle_t worker = nullptr;
    Origin origin = Origin::Button;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_.load() == Phase::Inactive) {
            return;
        }
        phase_ = Phase::Inactive;
        generation_.fetch_add(1);
        capture_requested_ = false;
        exit_requested_ = false;
        preempted_exit_ = false;
        capture_tone_finished_ = false;
        capture_tone_session_id_ = 0;
        tts_session_id_ = 0;
        voice_mcp_result_callback_ = {};
        origin = origin_.load();
        origin_ = Origin::Button;
        worker = task_handle_;
    }

    if (worker != nullptr) {
        xTaskNotifyGive(worker);
    }

    if (stop_audio) {
        application_->GetAudioPlayer().StopNonMusic(
            PlaybackEndReason::kCancelled);
    }
    Ui::DismissCameraPreview();
    Ui::SetHomeSubtitle({});
    Ui::DismissImage();
    if (origin == Origin::Button) {
        if (application_->GetDeviceState() != kDeviceStateIdle) {
            application_->SetDeviceState(kDeviceStateIdle);
        }
        Ui::SetStatus(Ui::Status::Standby);
        // 先切到待命状态，再恢复待机表情，避免 UI 把监听状态下的待机表情
        // 判定为无效更新而忽略。
        Ui::SetEmotion("待机");
    }
    ReleaseCameraIntent();
    ESP_LOGI(kTag, "退出拍照流程");
}

bool CameraService::IsCurrent(uint32_t generation) const {
    return generation != 0 && generation_.load() == generation &&
           phase_.load() != Phase::Inactive;
}
