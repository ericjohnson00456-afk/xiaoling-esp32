#include "camera_preview_controller.h"

#include "application.h"
#include "boards/common/board.h"
#include "boards/common/camera.h"
#include "player/intents/playback_intent.h"
#include "system/device_status.h"
#include "system/psram_task.h"
#include "ui/ui.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <array>
#include <exception>
#include <memory>
#include <utility>

namespace {

constexpr char kTag[] = "CameraPreview";
constexpr uint16_t kCaptureToneId = 73;
constexpr uint16_t kPreviewWidth = 160;
constexpr uint16_t kPreviewHeight = 120;
constexpr TickType_t kPreviewWaitInterval = pdMS_TO_TICKS(20);
constexpr TickType_t kCaptureToneWaitTimeout = pdMS_TO_TICKS(2000);
constexpr TickType_t kTtsStartWaitTimeout = pdMS_TO_TICKS(12000);
constexpr configSTACK_DEPTH_TYPE kWorkerStackSize = 12 * 1024;
constexpr UBaseType_t kWorkerPriority = 5;
constexpr int64_t kStartupGuardUs = 3 * 1000 * 1000;
constexpr char kVisionPrompt[] =
    "这个图片里面有什么东西";

}  // 匿名命名空间

CameraPreviewController& CameraPreviewController::GetInstance() {
    static CameraPreviewController controller;
    return controller;
}

CameraPreviewController::~CameraPreviewController() {
    for (const auto subscription : subscriptions_) {
        AppEvent::Unsubscribe(subscription);
    }
}

void CameraPreviewController::Initialize(Application& application) {
    if (application_ != nullptr) {
        return;
    }
    application_ = &application;
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

bool CameraPreviewController::IsActive() const {
    return phase_.load() != Phase::Inactive;
}

bool CameraPreviewController::Enter() {
    auto* camera = Board::GetInstance().GetCamera();
    if (application_ == nullptr || camera == nullptr || !camera->IsAvailable()) {
        ESP_LOGW(kTag, "双击拍照失败：未检测到摄像头");
        Ui::ShowToast("未检测到摄像头");
        return false;
    }
    if (esp_timer_get_time() < kStartupGuardUs) {
        ESP_LOGW(kTag, "启动保护期内忽略拍照预览请求");
        return false;
    }

    const DeviceState state = application_->GetDeviceState();
    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring ||
        state == kDeviceStateUpgrading || state == kDeviceStateFatalError) {
        ESP_LOGW(kTag, "当前状态不能进入拍照预览: %s",
                 DeviceStatus::GetStateName(state));
        Ui::ShowToast("当前状态暂时不能拍照");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_.load() != Phase::Inactive || task_handle_ != nullptr) {
        return false;
    }

    if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
        state == kDeviceStateConnecting) {
        application_->AbortSpeaking(kAbortReasonWakeWordDetected);
        application_->GetIntentManager().Pop(PlaybackIntentId::kVoiceSession);
        application_->SetDeviceState(kDeviceStateIdle);
    }

    resume_music_ = application_->GetAudioPlayer().IsMusicPlaying();
    if (resume_music_) {
        application_->GetAudioPlayer().PauseMusic();
    }
    application_->GetAudioPlayer().StopNonMusic(PlaybackEndReason::kPreempted);

    capture_requested_ = false;
    exit_requested_ = false;
    capture_tone_finished_ = false;
    capture_tone_session_id_ = 0;
    tts_session_id_ = 0;
    const uint32_t generation = generation_.fetch_add(1) + 1;
    phase_ = Phase::Preview;
    Ui::ShowCameraPreview();

    auto context = std::make_unique<TaskContext>();
    context->controller = this;
    context->generation = generation;
    if (PsramTask::Create(TaskEntry, "camera_preview", kWorkerStackSize,
                          context.get(), kWorkerPriority, &task_handle_,
                          &context->stack_in_psram) != pdPASS) {
        task_handle_ = nullptr;
        phase_ = Phase::Inactive;
        Ui::DismissCameraPreview();
        if (resume_music_) {
            application_->GetAudioPlayer().ResumeMusic();
            resume_music_ = false;
        }
        ESP_LOGE(kTag, "创建相机预览任务失败");
        return false;
    }
    context.release();
    ESP_LOGI(kTag, "进入按键拍照预览: generation=%lu",
             static_cast<unsigned long>(generation));
    return true;
}

bool CameraPreviewController::Capture() {
    if (phase_.load() != Phase::Preview) {
        return IsActive();
    }
    if (!capture_requested_.exchange(true)) {
        ESP_LOGI(kTag, "单击拍照请求已提交");
    }
    return true;
}

bool CameraPreviewController::Exit() {
    const Phase phase = phase_.load();
    if (phase == Phase::Inactive) {
        return false;
    }
    exit_requested_ = true;
    if (phase == Phase::Preview) {
        phase_ = Phase::Exiting;
        ESP_LOGI(kTag, "等待当前预览帧结束后退出");
    } else {
        FinishExit(true);
    }
    return true;
}

void CameraPreviewController::TaskEntry(void* arg) {
    std::unique_ptr<TaskContext> context(static_cast<TaskContext*>(arg));
    auto* controller = context->controller;
    const uint32_t generation = context->generation;
    const bool stack_in_psram = context->stack_in_psram;
    controller->TaskLoop(generation);
    {
        std::lock_guard<std::mutex> lock(controller->mutex_);
        controller->task_handle_ = nullptr;
    }
    context.reset();
    PsramTask::Delete(stack_in_psram);
}

void CameraPreviewController::TaskLoop(uint32_t generation) {
    auto* camera = Board::GetInstance().GetCamera();
    if (camera == nullptr || !camera->IsAvailable()) {
        Fail(generation, "未检测到摄像头");
        return;
    }

    bool preview_frame_ready = false;
    while (IsCurrent(generation)) {
        if (phase_.load() == Phase::Exiting || exit_requested_.load()) {
            FinishExit(true);
            return;
        }
        if (phase_.load() != Phase::Preview) {
            return;
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
            RequestCloudDescription(generation, *camera);
            if (!IsCurrent(generation) || phase_.load() != Phase::WaitingTts) {
                return;
            }

            // 残留云端会话或服务端瞬时异常可能导致会话结束但不下发 TTS URL。
            // 此时用冻结帧重试一次；请求入口会先取消旧会话再启动新会话。
            ulTaskNotifyTake(pdTRUE, kTtsStartWaitTimeout);
            if (IsCurrent(generation) && phase_.load() == Phase::WaitingTts) {
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

void CameraPreviewController::RequestCloudDescription(uint32_t generation,
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

void CameraPreviewController::HandlePlaybackEvent(const PlaybackEvent& event) {
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

void CameraPreviewController::Fail(uint32_t generation, const char* message) {
    if (!IsCurrent(generation)) {
        return;
    }
    phase_ = Phase::Processing;
    ESP_LOGE(kTag, "%s", message);
    Ui::ShowToast(message, 3000);
    FinishExit(true);
}

void CameraPreviewController::FinishExit(bool stop_audio) {
    bool resume_music = false;
    TaskHandle_t worker = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_.load() == Phase::Inactive) {
            return;
        }
        phase_ = Phase::Inactive;
        generation_.fetch_add(1);
        capture_requested_ = false;
        exit_requested_ = false;
        capture_tone_finished_ = false;
        capture_tone_session_id_ = 0;
        tts_session_id_ = 0;
        resume_music = resume_music_;
        resume_music_ = false;
        worker = task_handle_;
    }

    if (worker != nullptr) {
        xTaskNotifyGive(worker);
    }

    if (stop_audio) {
        application_->GetAudioPlayer().StopNonMusic(
            PlaybackEndReason::kCancelled);
    }
    if (application_->GetDeviceState() != kDeviceStateIdle) {
        application_->SetDeviceState(kDeviceStateIdle);
    }
    Ui::DismissCameraPreview();
    Ui::SetHomeSubtitle({});
    Ui::DismissImage();
    Ui::SetStatus(Ui::Status::Standby);
    // 先切到待命状态，再恢复待机表情，避免 UI 把监听状态下的待机表情
    // 判定为无效更新而忽略。
    Ui::SetEmotion("待机");
    if (resume_music) {
        application_->GetAudioPlayer().ResumeMusic();
    }
    ESP_LOGI(kTag, "退出按键拍照流程");
}

bool CameraPreviewController::IsCurrent(uint32_t generation) const {
    return generation != 0 && generation_.load() == generation &&
           phase_.load() != Phase::Inactive;
}
