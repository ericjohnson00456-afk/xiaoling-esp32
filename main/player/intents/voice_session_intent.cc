#include "voice_session_intent.h"

#include "player/audio_player.h"

#include <esp_log.h>

#include <utility>

#define TAG "VoiceSessionIntent"

PlaybackIntentStack::Handlers CreateVoiceSessionIntentHandlers(
    AudioPlayer& audio_player, std::function<bool()> has_active_tts) {
    PlaybackIntentStack::Handlers handlers;
    handlers.on_enter = []() {
        ESP_LOGI(TAG, "语音会话事件进入");
    };
    handlers.on_preempted = [&audio_player, has_active_tts = std::move(has_active_tts)]() {
        if (has_active_tts && has_active_tts()) {
            ESP_LOGI(TAG, "语音会话被高优先级事件抢占，停止当前 TTS");
            audio_player.StopNonMusic(PlaybackEndReason::kPreempted);
        }
    };
    handlers.on_resumed = []() {
        ESP_LOGI(TAG, "高优先级事件结束，语音会话恢复为可收尾状态");
    };
    handlers.on_exit = []() {
        ESP_LOGI(TAG, "语音会话事件退出");
    };
    return handlers;
}
