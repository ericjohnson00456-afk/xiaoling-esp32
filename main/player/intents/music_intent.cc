#include "music_intent.h"

#include "player/audio_player.h"

#include <esp_log.h>

#include <utility>

#define TAG "MusicIntent"

PlaybackIntentStack::Handlers CreateMusicIntentHandlers(
    AudioPlayer& audio_player, std::function<bool()> should_resume) {
    PlaybackIntentStack::Handlers handlers;
    handlers.on_preempted = [&audio_player]() {
        if (!audio_player.PauseMusic()) {
            ESP_LOGW(TAG, "音乐事件被抢占，但暂停音乐失败");
        } else {
            ESP_LOGI(TAG, "音乐事件被高优先级事件抢占，音乐已暂停");
        }
    };
    handlers.on_resumed = [&audio_player, should_resume = std::move(should_resume)]() {
        if (!should_resume || !should_resume()) {
            return;
        }
        if (audio_player.ResumeMusic()) {
            ESP_LOGI(TAG, "音乐事件恢复播放");
        } else {
            ESP_LOGW(TAG, "音乐事件恢复失败，当前曲目可能已结束");
        }
    };
    return handlers;
}
