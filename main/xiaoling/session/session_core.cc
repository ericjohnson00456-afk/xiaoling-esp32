#include "xiaoling/session/session_core.h"

DuplexPolicy DuplexPolicy::ForMode(DuplexMode mode) {
    DuplexPolicy p;
    p.mode = mode;
    switch (mode) {
    case DuplexMode::kSimplex:
        p.allow_barge_in = false;
        break;
    case DuplexMode::kHalfDuplex:
        break;
    case DuplexMode::kFullDuplex:
        p.mic_during_tts = true;
        p.auto_restart_listening = true;
        break;
    }
    return p;
}

void VoiceSessionController::SetMode(DuplexMode mode) {
    policy_ = DuplexPolicy::ForMode(mode);
}

const DuplexPolicy& VoiceSessionController::policy() const {
    return policy_;
}

void VoiceSessionController::StartListening(bool realtime) {
    realtime_session_ = realtime;
}

void VoiceSessionController::EndSession() {
    realtime_session_ = false;
    active_tts_ = 0;
}

bool VoiceSessionController::KeepListeningDuringTts() const {
    return policy_.mic_during_tts && realtime_session_;
}

void VoiceSessionController::StartTts(uint32_t id) {
    active_tts_ = id;
}

bool VoiceSessionController::EndTts(uint32_t id) {
    if (id == 0 || id != active_tts_) return false;
    active_tts_ = 0;
    return true;
}

void VoiceSessionController::InvalidateTts() {
    active_tts_ = 0;
}
