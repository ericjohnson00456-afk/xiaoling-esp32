#pragma once
#include <cstdint>

// Policy shared by the application and audio service.  ListeningMode describes
// the server protocol; DuplexMode describes the cloud microphone/speaker gate.
// Hardware AEC stays enabled independently of this policy.
enum class DuplexMode { kSimplex, kHalfDuplex, kFullDuplex };

struct DuplexPolicy {
    DuplexMode mode = DuplexMode::kHalfDuplex;
    bool mic_during_tts = false;
    bool allow_barge_in = true;
    bool auto_restart_listening = false;

    static DuplexPolicy ForMode(DuplexMode mode);
};

class VoiceSessionController {
public:
    void SetMode(DuplexMode mode);
    const DuplexPolicy& policy() const;
    void StartListening(bool realtime);
    void EndSession();
    bool KeepListeningDuringTts() const;
    void StartTts(uint32_t id);
    bool EndTts(uint32_t id);
    void InvalidateTts();
private:
    DuplexPolicy policy_ = DuplexPolicy::ForMode(DuplexMode::kHalfDuplex);
    bool realtime_session_ = false;
    uint32_t active_tts_ = 0;
};
