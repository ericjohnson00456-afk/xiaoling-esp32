#include "application.h"

#include "alarm/alarm_service.h"
#include "audio_codec.h"
#include "board.h"
#include "shell/cmd/kv/kv_user.h"
#include "xiaoling/xiaoling_events.h"

#include <esp_log.h>

#include <utility>

namespace {

constexpr char kTag[] = "ApplicationAlarm";

}  // namespace

Application::AlarmFlowState::~AlarmFlowState() {
    if (connected_subscription_id != 0) {
        XiaolingEventBus::GetInstance().Unsubscribe(connected_subscription_id);
    }
}

void Application::InitializeAlarm() {
    if (alarm_flow_.initialized) {
        return;
    }
    alarm_flow_.initialized = true;

    // AlarmPlayer 独占 kAlarm 通道，播放事件直接交给闹钟业务推进下一阶段。
    audio_player_.SetAlarmEventCallback([](const PlaybackEvent& event) {
        AlarmService::GetInstance().HandlePlaybackEvent(event);
    });
    // 复用 Application 的每秒时钟事件，不额外创建闹钟定时器。
    alarm_flow_.on_clock_tick = []() {
        AlarmService::GetInstance().Tick();
    };
    // 唤醒词应先关闭当前闹钟，再继续正常唤醒会话。
    alarm_flow_.on_wake_word = []() {
        auto& alarm = AlarmService::GetInstance();
        if (alarm.IsRinging()) {
            alarm.Stop();
        }
    };
    // 小聆鉴权和 WebSocket 协议握手完成后，再开始云端闹钟全量对账。
    alarm_flow_.connected_subscription_id = XiaolingEventBus::GetInstance().Subscribe(
        XIAOLING_CONNECTED, [](const XiaolingEvent&) {
            AlarmService::GetInstance().RequestCloudSync();
        });

    // 本地闹钟不依赖云端鉴权；NVS 就绪后立即恢复，校时完成后自动开始调度。
    if (!AlarmService::GetInstance().Initialize()) {
        ESP_LOGW(kTag, "本地闹钟数据恢复失败");
    }
}

uint32_t Application::StartAlarmTone(uint16_t tone_id) {
    PrepareAlarmPlayback();
    const uint32_t session_id = audio_player_.PlayAlarmTone(tone_id);
    if (session_id == 0) {
        ESP_LOGW(kTag, "闹钟提示音播放失败: id=%u", static_cast<unsigned>(tone_id));
    }
    return session_id;
}

uint32_t Application::StartAlarmTtsUrl(const std::string& url) {
    PrepareAlarmPlayback();
    const uint32_t session_id = audio_player_.PlayAlarmTtsUrl(url);
    if (session_id == 0) {
        ESP_LOGW(kTag, "闹钟 TTS URL 播放失败");
    }
    return session_id;
}

void Application::PrepareAlarmPlayback() {
    const bool already_ringing = playback_intents_.Contains(PlaybackIntent::kAlarm);
    if (!already_ringing) {
        if (auto* codec = Board::GetInstance().GetAudioCodec(); codec != nullptr) {
            const int minimum_ring_volume = KvUser::GetMinAlarmVolume();
            const int current_volume = codec->output_volume();
            if (current_volume < minimum_ring_volume) {
                alarm_flow_.previous_volume = current_volume;
                codec->SetOutputVolumeTemporary(minimum_ring_volume);
                ESP_LOGI(kTag, "闹钟临时音量: %d -> %d", current_volume,
                         minimum_ring_volume);
            } else {
                // 配置值是音量下限，并非固定的响铃音量。
                alarm_flow_.previous_volume = -1;
                ESP_LOGI(kTag, "闹钟保持当前音量: %d (minimum=%d)", current_volume,
                         minimum_ring_volume);
            }
        }
    }
    playback_intents_.Push(PlaybackIntent::kAlarm);
}

bool Application::RequestAlarmTtsUrl(const std::string& text) {
    std::lock_guard<std::mutex> lock(xiaoling_conn_mutex_);
    if (xiaoling_conn_ == nullptr) {
        return false;
    }
    return xiaoling_conn_->RequestAlarmTtsUrl(
        text, [](bool success, std::string url) {
            auto& app = Application::GetInstance();
            app.Schedule([success, url = std::move(url)]() mutable {
                AlarmService::GetInstance().HandleTtsUrl(success, std::move(url));
            });
        });
}

void Application::CancelAlarmTtsRequest() {
    std::lock_guard<std::mutex> lock(xiaoling_conn_mutex_);
    if (xiaoling_conn_ != nullptr) {
        xiaoling_conn_->CancelAlarmTtsRequest();
    }
}

bool Application::FetchCloudAlarmList(std::string& response_body) {
    std::lock_guard<std::mutex> lock(xiaoling_conn_mutex_);
    return xiaoling_conn_ != nullptr && xiaoling_conn_->FetchAlarmList(response_body);
}

bool Application::ResolveCustomAlarmDate(const std::string& alarm_id,
                                         std::string& date) {
    std::lock_guard<std::mutex> lock(xiaoling_conn_mutex_);
    return xiaoling_conn_ != nullptr &&
           xiaoling_conn_->ResolveCustomAlarmDate(alarm_id, date);
}

void Application::StopAlarmTone() {
    audio_player_.StopNonMusic(PlaybackEndReason::kCancelled);
    playback_intents_.Pop(PlaybackIntent::kAlarm);
    if (alarm_flow_.previous_volume >= 0) {
        if (auto* codec = Board::GetInstance().GetAudioCodec(); codec != nullptr) {
            codec->SetOutputVolumeTemporary(alarm_flow_.previous_volume);
        }
        alarm_flow_.previous_volume = -1;
    }
}
