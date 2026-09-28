#include "alarm_intent.h"

#include <esp_log.h>

namespace {

constexpr char kTag[] = "AlarmIntent";

}  // namespace

PlaybackIntentStack::Handlers CreateAlarmIntentHandlers() {
    PlaybackIntentStack::Handlers handlers;
    handlers.on_enter = []() {
        ESP_LOGI(kTag, "闹钟事件进入，取得播放优先级");
    };
    handlers.on_preempted = []() {
        ESP_LOGI(kTag, "闹钟事件被更高优先级事件抢占");
    };
    handlers.on_resumed = []() {
        ESP_LOGI(kTag, "高优先级事件结束，恢复闹钟事件");
    };
    handlers.on_exit = []() {
        ESP_LOGI(kTag, "闹钟事件退出");
    };
    return handlers;
}
