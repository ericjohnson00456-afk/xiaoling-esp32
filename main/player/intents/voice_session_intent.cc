#include "voice_session_intent.h"

#include "application.h"

#include <esp_log.h>

namespace {

constexpr char kTag[] = "VoiceSessionIntent";

} // namespace

void VoiceSessionIntent::Initialize(Application &app) {
  app_ = &app;
}

void VoiceSessionIntent::OnEnter() { ESP_LOGI(kTag, "语音会话事件进入"); }

void VoiceSessionIntent::OnPreempted() {
  if (app_ != nullptr &&
      (app_->active_tts_session_id_ != 0 ||
       app_->tts_waiting_for_playback_.load())) {
    ESP_LOGI(kTag, "语音会话被高优先级事件抢占，停止当前 TTS");
    app_->audio_player_.StopNonMusic(PlaybackEndReason::kPreempted);
  }
}

void VoiceSessionIntent::OnResumed() {
  ESP_LOGI(kTag, "高优先级事件结束，语音会话恢复为可收尾状态");
}

void VoiceSessionIntent::OnExit() { ESP_LOGI(kTag, "语音会话事件退出"); }
