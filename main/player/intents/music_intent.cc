#include "music_intent.h"

#include "player/audio_player.h"

#include <esp_log.h>

#include <utility>

#define TAG "MusicIntent"

PlaybackIntentStack::Handlers CreateMusicIntentHandlers(
    AudioPlayer& audio_player, std::function<bool()> should_resume,
    std::function<bool()> start_playback,
    std::function<void()> clear_pending_list) {
    PlaybackIntentStack::Handlers handlers;
    handlers.on_enter = [start_playback = std::move(start_playback)]() {
        if (start_playback && start_playback()) {
            ESP_LOGI(TAG, "音乐事件进入，播放列表已缓存并开始播放");
        } else {
            ESP_LOGW(TAG, "音乐事件进入，但没有可提交的播放列表");
        }
    };
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
    handlers.on_exit = [&audio_player,
                        clear_pending_list = std::move(clear_pending_list)]() {
        audio_player.ClearMusicList();
        if (clear_pending_list) {
            clear_pending_list();
        }
        ESP_LOGI(TAG, "音乐事件退出，播放列表已清除");
    };
    return handlers;
}
