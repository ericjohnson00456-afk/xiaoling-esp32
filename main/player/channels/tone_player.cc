#include "tone_player.h"

#include "engine/player_engine.h"
#include "resource/tone_model.h"

#include <esp_log.h>
#include <esp_random.h>

#include <utility>

TonePlayer::TonePlayer(PlayerEngine& engine, SessionAllocator allocate_session)
    : engine_(&engine), allocate_session_(std::move(allocate_session)) {}

uint32_t TonePlayer::Submit(const PlaybackRequest& request, PlayerEngine& engine) {
    return engine.Play(request) ? request.session_id : 0;
}

uint32_t TonePlayer::PlayTone(uint16_t tone_id) {
    const char* url = ToneModel::GetInstance().GetUrl(tone_id);
    if (url == nullptr) {
        ESP_LOGW("TonePlayer", "tone 不存在: id=%u", static_cast<unsigned>(tone_id));
        return 0;
    }
    PlaybackRequest request;
    request.session_id = allocate_session_();
    request.source_type = PlaybackSourceType::kRomfs;
    request.channel = PlaybackChannel::kPrompt;
    request.source = url;
    request.priority = 80;
    return Submit(request, *engine_);
}

uint32_t TonePlayer::PlayAlarm(uint16_t tone_id) {
    const char* url = ToneModel::GetInstance().GetUrl(tone_id);
    if (url == nullptr) {
        ESP_LOGW("TonePlayer", "闹钟 tone 不存在: id=%u", static_cast<unsigned>(tone_id));
        return 0;
    }
    PlaybackRequest request;
    request.session_id = allocate_session_();
    request.source_type = PlaybackSourceType::kRomfs;
    request.channel = PlaybackChannel::kAlarm;
    request.source = url;
    request.priority = 100;
    return Submit(request, *engine_);
}

std::optional<uint16_t> TonePlayer::SelectWakeupTone() {
    auto& tones = ToneModel::GetInstance();
    uint16_t candidates[5] = {};
    size_t count = 0;
    for (uint16_t tone_id = 0; tone_id <= 4; ++tone_id) {
        if (tones.GetUrl(tone_id) != nullptr && tone_id != last_wakeup_tone_) {
            candidates[count++] = tone_id;
        }
    }
    if (count == 0) {
        if (last_wakeup_tone_ && tones.GetUrl(*last_wakeup_tone_) != nullptr) {
            return last_wakeup_tone_;
        }
        return std::nullopt;
    }
    return candidates[esp_random() % count];
}

void TonePlayer::PlayWakeup(std::function<void(bool)> completion) {
    CancelWakeup();
    completion_ = std::move(completion);
    const auto tone_id = SelectWakeupTone();
    if (!tone_id) {
        ESP_LOGW("TonePlayer", "No ROMFS wakeup tone; continue listening");
        CompleteWakeup(true);
        return;
    }
    wakeup_session_id_ = PlayTone(*tone_id);
    if (wakeup_session_id_ == 0) {
        ESP_LOGW("TonePlayer", "Wakeup tone failed to start; continue listening");
        CompleteWakeup(true);
        return;
    }
    last_wakeup_tone_ = tone_id;
    ESP_LOGI("TonePlayer", "Wakeup tone: id=%u, session=%lu",
             static_cast<unsigned>(*tone_id), static_cast<unsigned long>(wakeup_session_id_));
}

void TonePlayer::OnEvent(const PlaybackEvent& event) {
    if (!completion_ || event.channel != PlaybackChannel::kPrompt ||
        event.session_id != wakeup_session_id_) {
        return;
    }
    if (event.state == PlaybackState::kCompleted || event.state == PlaybackState::kFailed) {
        CompleteWakeup(true);
    } else if (event.state == PlaybackState::kStopped) {
        CompleteWakeup(false);
    }
}

void TonePlayer::CompleteWakeup(bool proceed) {
    wakeup_session_id_ = 0;
    auto completion = std::move(completion_);
    completion_ = {};
    if (completion) {
        completion(proceed);
    }
}

void TonePlayer::CancelWakeup() {
    const bool was_playing = wakeup_session_id_ != 0;
    wakeup_session_id_ = 0;
    completion_ = {};
    if (was_playing) {
        engine_->Stop(PlaybackEndReason::kCancelled);
    }
}

void TonePlayer::Cancel(PlayerEngine& engine) {
    CancelWakeup();
    engine.Stop(PlaybackEndReason::kCancelled);
}
