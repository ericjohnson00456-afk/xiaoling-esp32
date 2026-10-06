#include "camera_capture_intent.h"

#include "application.h"
#include "playback_intent_manager.h"

#include <esp_log.h>

#include <utility>

namespace {

constexpr char kTag[] = "CameraCaptureIntent";

bool IsConversationState(DeviceState state) {
  return state == kDeviceStateConnecting || state == kDeviceStateListening ||
         state == kDeviceStateSpeaking;
}

}  // 匿名命名空间

void CameraCaptureIntent::Initialize(PlaybackIntentManager &intent_manager,
                                     Application &app) {
  intent_manager_ = &intent_manager;
  app_ = &app;
}

void CameraCaptureIntent::SetCancelCallback(CancelCallback callback) {
  cancel_callback_ = std::move(callback);
}

bool CameraCaptureIntent::Enter() {
  if (app_ == nullptr || intent_manager_ == nullptr) {
    return false;
  }
  if (active_.load()) {
    // 上一轮正在异步出栈时拒绝新流程，避免旧的退出任务把新拍照 Intent 弹出。
    return !exit_pending_.load() &&
           intent_manager_->Top() == PlaybackIntentId::kCameraCapture;
  }

  exit_pending_ = false;
  cancel_pending_ = false;
  active_ = true;
  if (!intent_manager_->Push(PlaybackIntentId::kCameraCapture)) {
    active_ = false;
    return false;
  }
  if (intent_manager_->Top() != PlaybackIntentId::kCameraCapture) {
    intent_manager_->Pop(PlaybackIntentId::kCameraCapture);
    active_ = false;
    return false;
  }

  // 拍照是一次性流程，结束后应回到待命而不是恢复此前的语音会话；音乐
  // 则继续留在栈底，由拍照 Intent 退出时自动恢复。
  intent_manager_->Pop(PlaybackIntentId::kVoiceSession);
  return true;
}

void CameraCaptureIntent::Exit() {
  if (!active_.load() || exit_pending_.exchange(true) || app_ == nullptr) {
    return;
  }

  // 拍照失败可能来自摄像头工作任务，Intent 栈只允许在 Application 主任务
  // 中修改，因此统一异步出栈。
  app_->Schedule([this]() {
    if (active_.load() && intent_manager_ != nullptr) {
      intent_manager_->Pop(PlaybackIntentId::kCameraCapture);
    }
    active_ = false;
    exit_pending_ = false;
    cancel_pending_ = false;
  });
}

void CameraCaptureIntent::OnEnter() {
  if (app_ == nullptr) {
    return;
  }
  const DeviceState state = app_->GetDeviceState();
  if (IsConversationState(state)) {
    app_->AbortSpeaking(kAbortReasonWakeWordDetected);
  } else {
    app_->GetAudioPlayer().StopNonMusic(PlaybackEndReason::kPreempted);
  }
  if (app_->GetDeviceState() != kDeviceStateIdle) {
    app_->SetDeviceState(kDeviceStateIdle);
  }
  ESP_LOGI(kTag, "拍照事件进入，取得播放优先级");
}

void CameraCaptureIntent::OnPreempted() {
  ESP_LOGI(kTag, "拍照事件被更高优先级事件抢占，取消当前拍照流程");
  CancelCapture();
}

void CameraCaptureIntent::OnResumed() {
  // 拍照上传和 TTS 均不可从中间状态安全恢复，被抢占后继续确保流程退出。
  ESP_LOGI(kTag, "拍照事件不可恢复，继续执行取消流程");
  CancelCapture();
}

void CameraCaptureIntent::OnExit() {
  active_ = false;
  cancel_pending_ = false;
  ESP_LOGI(kTag, "拍照事件退出，允许恢复低优先级播放");
}

void CameraCaptureIntent::CancelCapture() {
  if (!cancel_callback_ || cancel_pending_.exchange(true)) {
    return;
  }
  // 回调只标记业务退出，真正的 Intent 出栈仍由 Exit() 投递到 Application
  // 主任务，避免在优先级栈回调中递归修改栈结构。
  if (active_.load()) {
    cancel_callback_();
  }
  cancel_pending_ = false;
}
