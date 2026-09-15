#include "player_tone.h"

#include "audio_player.h"
#include "audio/tone/tone_model.h"

#include <esp_log.h>
#include <esp_random.h>
#include <utility>

std::optional<uint16_t> PlayerTone::SelectWakeupTone() {
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

void PlayerTone::PlayWakeup(std::function<void(bool)> completion) {
    CancelWakeup();
    completion_ = std::move(completion);
    const auto tone_id = SelectWakeupTone();
    if (!tone_id) {
        ESP_LOGW("PlayerTone", "No ROMFS wakeup tone; continue listening");
        CompleteWakeup(true);
        return;
    }
    wakeup_session_id_ = player_.PlayTone(*tone_id);
    if (wakeup_session_id_ == 0) {
        ESP_LOGW("PlayerTone", "Wakeup tone failed to start; continue listening");
        CompleteWakeup(true);
        return;
    }
    last_wakeup_tone_ = tone_id;
    ESP_LOGI("PlayerTone", "Wakeup tone: id=%u, session=%lu",
             static_cast<unsigned>(*tone_id), static_cast<unsigned long>(wakeup_session_id_));
}

void PlayerTone::HandlePlaybackEvent(const PlaybackEvent& event) {
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

void PlayerTone::CompleteWakeup(bool proceed) {
    wakeup_session_id_ = 0;
    auto completion = std::move(completion_);
    completion_ = {};
    if (completion) {
        completion(proceed);
    }
}

void PlayerTone::CancelWakeup() {
    const bool was_playing = wakeup_session_id_ != 0;
    wakeup_session_id_ = 0;
    completion_ = {};
    if (was_playing) {
        player_.Stop(PlaybackEndReason::kCancelled);
    }
}
