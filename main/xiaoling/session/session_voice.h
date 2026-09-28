#ifndef XIAOLING_VOICE_H
#define XIAOLING_VOICE_H

#include "xiaoling_types.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

struct cJSON;
class TtsSubtitleFetcher;

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
    // HTTP 字幕流与 WebSocket 使用同一份鉴权信息。
    void SetAccessToken(std::string token);
    void OnIncomingAudio(IncomingAudioCallback callback);

    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet);
    bool SendAudioBatch(std::vector<std::unique_ptr<AudioStreamPacket>> packets);
    void SendWakeWordDetected(const std::string& wake_word);
    void SendStartListening(ListeningMode mode);
    void SendStopListening();
    bool EndSession();
    bool CancelSession();
    // 在停止本地 TTS 前冻结实际 PCM 播放位置对应的 timeline 游标。
    void PrepareAbortSpeaking(AbortReason reason);
    void SendAbortSpeaking(AbortReason reason);
    // 非空 IAT 触发的全双工打断：停止字幕并上报播放点，但保持会话和上行。
    void HandleIatBargeIn();

    void HandleMessage(const cJSON* root);
    void HandleBinary(std::vector<uint8_t> payload);
    void Reset();

    int server_sample_rate() const { return kServerSampleRate; }
    int server_frame_duration() const { return kServerFrameDurationMs; }
    const std::string& session_id() const { return session_id_; }
    bool audio_upload_enabled() const { return audio_upload_enabled_.load(); }

private:
    static constexpr int kServerSampleRate = 16000;
    static constexpr int kServerFrameDurationMs = 60;
    static constexpr int kSessionStartTimeoutMs = 3000;
    static constexpr int kOpusWbSampleRate = 16000;
    static constexpr int kOpusWbFrameDurationMs = 20;
    static constexpr size_t kOpusWbPayloadBytes = 40;
    static constexpr size_t kMaxTimelineSegments = 128;
    static constexpr EventBits_t kSessionStartedBit = BIT0;

    struct TimelineCursor {
        uint32_t index = 0;
        uint32_t start_index = 0;
        uint32_t end_index = 0;
        uint32_t start_ms = 0;
        uint32_t end_ms = 0;
    };

    struct FrozenInterruption {
        bool valid = false;
        std::string sid;
        TimelineCursor cursor;
        uint32_t playback_position_ms = 0;
    };

    EventGroupHandle_t event_group_ = nullptr;
    TextSender text_sender_;
    BinarySender binary_sender_;
    SessionStateCallback session_state_callback_;
    ErrorCallback error_callback_;
    IncomingAudioCallback on_incoming_audio_;

    std::atomic_bool session_active_{false};
    std::atomic_bool audio_upload_enabled_{false};
    bool narrowband_mode_ = false;
    std::string session_id_;
    std::string pending_wake_word_;
    std::deque<std::unique_ptr<AudioStreamPacket>> pending_wake_audio_;
    std::atomic_bool uplink_audio_format_checked_{false};
    std::atomic<uint32_t> uplink_batch_sequence_{0};
    std::atomic<uint32_t> uplink_rejected_batches_{0};

    bool iat_active_ = false;
    std::atomic_bool tts_text_active_{false};
    std::string last_iat_text_;
    std::unique_ptr<TtsSubtitleFetcher> subtitle_fetcher_;
    std::mutex subtitle_state_mutex_;
    std::string pending_subtitle_url_;
    bool tts_audio_pending_ = false;
    bool tts_pipeline_running_ = false;
    bool tts_audio_playing_ = false;
    uint32_t tts_audio_clock_ms_ = 0;
    bool timeline_negotiated_ = false;
    bool timeline_valid_ = false;
    bool timeline_reported_ = false;
    std::string reply_sid_;
    std::vector<TimelineCursor> timeline_cursors_;
    uint32_t expected_timeline_index_ = 0;
    uint32_t expected_start_index_ = 0;
    uint32_t previous_timeline_end_ms_ = 0;
    FrozenInterruption frozen_interruption_;
    uint32_t tts_stream_ready_subscription_id_ = 0;
    uint32_t tts_pipeline_subscription_id_ = 0;
    uint32_t tts_output_started_subscription_id_ = 0;
    uint32_t tts_progress_subscription_id_ = 0;
    uint32_t tts_stopped_subscription_id_ = 0;

    std::string BuildStartMessage(ListeningMode mode);
    bool FlushPendingWakeAudio();
    bool CheckUplinkAudioFormat(const AudioStreamPacket& packet);
    bool SendUplinkAudioPacket(const AudioStreamPacket& packet);
    void SetSessionActive(bool active);
    void SetAudioUploadEnabled(bool enabled, const char* reason);
    void ResetResultState();
    void HandleIat(const cJSON* data);
    void HandleTts(const cJSON* data, const std::string& reply_sid);
    void SetPendingSubtitleUrl(const std::string& url, const std::string& reply_sid);
    void StartTtsAudio(std::string url, const std::string& reply_sid);
    void StartPendingSubtitleStream();
    void OnTtsPipelinePlaying();
    void OnTtsOutputStarted();
    void OnTtsAudioProgress(uint32_t audio_clock_ms);
    void OnTtsPlaybackStopped(bool natural_end);
    void StartSubtitleStream(const std::string& url);
    void CancelSubtitleStream(bool notify_interrupt);
    void HandleSubtitleEvent(int event, const std::string& text);
    void ResetTimelineStateLocked();
    bool SendFrozenReplyInterruption();
    void EndIat();
    void EndTtsText();
};

#endif  // XIAOLING_VOICE_H
