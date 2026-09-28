#include "alarm_intent.h"

#include "audio_codec.h"
#include "board.h"
#include "playback_intent_manager.h"
#include "shell/cmd/kv/kv_user.h"

#include <esp_log.h>

namespace {

constexpr char kTag[] = "AlarmIntent";

} // namespace

void AlarmIntent::Initialize(PlaybackIntentManager &intent_manager) {
  intent_manager_ = &intent_manager;
}

bool AlarmIntent::Enter() {
  return intent_manager_ != nullptr &&
         intent_manager_->Push(PlaybackIntentId::kAlarm);
}

bool AlarmIntent::Exit() {
  return intent_manager_ != nullptr &&
         intent_manager_->Pop(PlaybackIntentId::kAlarm);
}

void AlarmIntent::OnEnter() {
  previous_volume_ = -1;
  if (auto *codec = Board::GetInstance().GetAudioCodec(); codec != nullptr) {
    const int minimum_ring_volume = KvUser::GetMinAlarmVolume();
    const int current_volume = codec->output_volume();
    if (current_volume < minimum_ring_volume) {
      previous_volume_ = current_volume;
      codec->SetOutputVolumeTemporary(minimum_ring_volume);
      ESP_LOGI(kTag, "闹钟临时音量: %d -> %d", current_volume,
               minimum_ring_volume);
    } else {
      ESP_LOGI(kTag, "闹钟保持当前音量: %d (minimum=%d)", current_volume,
               minimum_ring_volume);
    }
  }
  ESP_LOGI(kTag, "闹钟事件进入，取得播放优先级");
}

void AlarmIntent::OnPreempted() {
  ESP_LOGI(kTag, "闹钟事件被更高优先级事件抢占");
}

void AlarmIntent::OnResumed() {
  ESP_LOGI(kTag, "高优先级事件结束，恢复闹钟事件");
}

void AlarmIntent::OnExit() {
  if (previous_volume_ >= 0) {
    if (auto *codec = Board::GetInstance().GetAudioCodec(); codec != nullptr) {
      codec->SetOutputVolumeTemporary(previous_volume_);
    }
    previous_volume_ = -1;
  }
  ESP_LOGI(kTag, "闹钟事件退出");
}
