#ifndef XIAOLING_VOICE_H
#define XIAOLING_VOICE_H

#include "xiaoling_types.h"
#include "voice_session_epoch.h"

#include <atomic>
#include <cstdint>
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
    // HTTP 字幕流与 WebSocket 使用同一份鉴权信息。
    void SetAccessToken(std::string token);
    void OnIncomingAudio(IncomingAudioCallback callback);

    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet);
    bool SendAudioBatch(std::vector<std::unique_ptr<AudioStreamPacket>> packets);
    void SendWakeWordDetected(const std::string& wake_word);
    // 为按键图片识别准备无音频上行的文本会话，并清理上一轮语音结果状态。
    void PrepareTextSession(std::string session_id);
    void SendStartListening(ListeningMode mode);
    // 使用应用现有的一秒时钟检查，等待 started 不得阻塞唤醒主任务。
    void CheckStartTimeout();
    void SendStopListening();
    // 半双工收到 TTS 后只结束上行，保留当前回复的音频和字幕状态。
    bool StopListeningForTts();
    // 退出工具只关闭音频上报，保留云端会话以接收告别 TTS 和字幕。
    void DisableAudioUploadForExit();
    bool EndSession();
    bool CancelSession();
    // 在停止本地 TTS 前冻结实际 PCM 播放位置对应的 timeline 游标。
    void PrepareAbortSpeaking(AbortReason reason);
    void SendAbortSpeaking(AbortReason reason);
    // 非空 IAT 触发的全双工打断：停止字幕并上报播放点，但保持会话和上行。
    void HandleIatBargeIn();
    // MCP 拍照会停止早到的空 TTS 流并在工具响应后重播；期间保留字幕状态。
    void PrepareTtsReplay();

    bool HandleMessage(const cJSON* root, uint32_t* accepted_generation = nullptr);
    void HandleBinary(std::vector<uint8_t> payload);
    void Reset();

    int server_sample_rate() const { return kServerSampleRate; }
    int server_frame_duration() const { return kServerFrameDurationMs; }
    std::string session_id() const;
    uint32_t event_generation() const { return event_generation_.load(); }
    bool AcceptsSessionMessage(const std::string& rid) const;
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
    std::string session_id_;
    mutable std::recursive_mutex session_mutex_;
    std::atomic<uint32_t> event_generation_{Xiaoling::voice_session_epoch.Current()};
    bool accepting_session_messages_ = false;
    bool awaiting_started_ = false;
    int64_t start_deadline_us_ = 0;
    bool text_session_ = false;
    std::string pending_wake_word_;
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
    bool timeline_subtitle_stream_ = false;
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
    uint32_t tts_started_subscription_id_ = 0;
    uint32_t tts_progress_subscription_id_ = 0;
    uint32_t tts_stopped_subscription_id_ = 0;

    std::string BuildStartMessage(ListeningMode mode);
    bool CheckUplinkAudioFormat(const AudioStreamPacket& packet);
    bool SendUplinkAudioPacket(const AudioStreamPacket& packet);
    void SetSessionActive(bool active);
    void SetAudioUploadEnabled(bool enabled, const char* reason);
    void ResetResultState();
    void HandleIat(const cJSON* data);
    void HandleTts(const cJSON* data, const std::string& reply_sid);
    void SetPendingSubtitleUrl(const std::string& url, const std::string& reply_sid,
                               bool timeline_stream);
    void StartTtsAudio(std::string url, const std::string& reply_sid);
    void StartPendingSubtitleStream();
    void OnTtsPipelinePlaying();
    void OnTtsStarted();
    void OnTtsAudioProgress(uint32_t audio_clock_ms);
    void OnTtsPlaybackStopped(bool natural_end);
    void StartSubtitleStream(const std::string& url);
    void CancelSubtitleStream(bool notify_interrupt);
    void HandleSubtitleEvent(int event, const std::string& text, uint32_t generation);
    void ResetTimelineStateLocked();
    bool SendFrozenReplyInterruption();
    void EndIat();
    void EndTtsText();
};

#endif  // XIAOLING_VOICE_H
