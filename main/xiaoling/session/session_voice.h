#ifndef XIAOLING_VOICE_H
#define XIAOLING_VOICE_H

#include "xiaoling_types.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

struct cJSON;

enum class XiaolingSessionVoiceError {
    kServerError,
    kServerTimeout,
};

// 小聆语音会话层：负责会话控制、上下行音频和 IAT/TTS 事件转换。
class XiaolingSessionVoice final {
public:
    using TextSender = std::function<bool(const std::string&)>;
    using BinarySender = std::function<bool(const uint8_t*, size_t)>;
    using SessionStateCallback = std::function<void(bool)>;
    using ErrorCallback = std::function<void(XiaolingSessionVoiceError)>;
    using IncomingAudioCallback =
        std::function<void(std::unique_ptr<AudioStreamPacket>)>;

    XiaolingSessionVoice();
    ~XiaolingSessionVoice();

    void SetTransport(TextSender text_sender,
                      BinarySender binary_sender,
                      SessionStateCallback session_state_callback,
                      ErrorCallback error_callback);
    void SetNarrowbandMode(bool enabled);
    void OnIncomingAudio(IncomingAudioCallback callback);

    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet);
    void SendWakeWordDetected(const std::string& wake_word);
    void SendStartListening(ListeningMode mode);
    void SendStopListening();
    bool EndSession();
    void SendAbortSpeaking(AbortReason reason);

    void HandleMessage(const cJSON* root);
    void HandleBinary(std::vector<uint8_t> payload);
    void Reset();

    int server_sample_rate() const { return kServerSampleRate; }
    int server_frame_duration() const { return kServerFrameDurationMs; }
    const std::string& session_id() const { return session_id_; }

private:
    static constexpr int kServerSampleRate = 16000;
    static constexpr int kServerFrameDurationMs = 60;
    static constexpr int kSessionStartTimeoutMs = 3000;
    static constexpr int kOpusWbSampleRate = 16000;
    static constexpr int kOpusWbFrameDurationMs = 20;
    static constexpr size_t kOpusWbPayloadBytes = 40;
    static constexpr EventBits_t kSessionStartedBit = BIT0;

    EventGroupHandle_t event_group_ = nullptr;
    TextSender text_sender_;
    BinarySender binary_sender_;
    SessionStateCallback session_state_callback_;
    ErrorCallback error_callback_;
    IncomingAudioCallback on_incoming_audio_;

    std::atomic_bool session_active_{false};
    std::atomic_bool audio_upload_enabled_{false};
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    bool narrowband_mode_ = false;
    std::string session_id_;
    std::string pending_wake_word_;
    std::deque<std::unique_ptr<AudioStreamPacket>> pending_wake_audio_;
    bool uplink_audio_format_checked_ = false;

    bool iat_active_ = false;
    bool tts_text_active_ = false;
    std::string last_iat_text_;
    std::string last_tts_text_;

    std::string BuildStartMessage(ListeningMode mode);
    bool FlushPendingWakeAudio();
    bool CheckUplinkAudioFormat(const AudioStreamPacket& packet);
    bool SendUplinkAudioPacket(const AudioStreamPacket& packet);
    void SetSessionActive(bool active);
    void ResetResultState();
    void HandleIat(const cJSON* data);
    void HandleTts(const cJSON* data);
    void EndIat();
    void EndTtsText();
};

#endif  // XIAOLING_VOICE_H
