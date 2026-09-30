#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <model_path.h>

#include <opus_decoder.h>
#include <opus_resampler.h>

#include "audio_codec.h"
#include "audio_processor.h"
#include "opus_encoder/opus_wb_encoder.h"
#include "processors/audio_debugger.h"
#include "player/core/playback_event.h"
#include "wake_word.h"
#include "wake_words/micro_wake_word/config.h"
#include "xiaoling/xiaoling_types.h"


/*
 * AudioService 只维护两条固定数据链：
 * 1. ADC -> 16 kHz PCM -> AFE/AEC -> Opus 最新窗口
 * 2. 播放 PCM/下行 Opus -> PCM -> DAC
 *
 * Start() 后采集、算法和 Opus 编码任务常驻。业务层只通过
 * EnableAudioUpload() 决定是否消费新 Opus，不参与底层任务生命周期。
 */

// lschat 的 opus-wb 使用 20 ms 分包，每包包含 320 个 16 kHz 样本。
#define OPUS_FRAME_DURATION_MS 20
/* AEC 每 20 ms 输出一帧。编码任务短暂繁忙时最多保留 160 ms，队列满后
 * 丢弃最旧帧，不能反向阻塞采集和 AEC。 */
#define MAX_ENCODE_TASKS_IN_QUEUE 8
/* Keep the normal server-downlink queue small so interactive Opus playback
 * remains responsive. Network TTS uses a separate, deeper limit below. */
#define MAX_PLAYBACK_TASKS_IN_QUEUE 8
/* TTS HTTP delivers MP3 in bursty chunks. Keep enough decoded PCM to absorb
 * multi-second gaps between network reads; the queue is allocated in PSRAM
 * on this board. Short clips bypass the gate when the stream finishes. */
#define MAX_PLAYER_PLAYBACK_TASKS_IN_QUEUE 192
/* Two seconds of 16 kHz, mono, 16-bit PCM is 64 KiB. Start TTS as soon as
 * that amount is decoded; short streams are released at EOF. */
#define PLAYER_INITIAL_PREBUFFER_BYTES (4 * 1024)
#define TTS_INITIAL_PREBUFFER_BYTES (64 * 1024)
/* After a mid-stream underrun, resume as soon as any PCM block arrives. */
#define PLAYER_RECOVERY_PREBUFFER_BYTES 1
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
/* AudioService 始终生产 Opus，只保留最近 2.4 秒，业务未上报时覆盖旧包。 */
#define MAX_OPUS_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
// Wake inference must never build a stale backlog while AEC/TTS are active.
// Keep only the newest couple of frames; overflow drops the oldest one.
#define MAX_PROCESSED_WAKE_FRAMES_IN_QUEUE 2
#define AUDIO_TESTING_MAX_DURATION_MS 10000

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000


struct AudioServiceCallbacks {
    /* 只在业务打开音频上报后通知；Opus 的生产本身不受此开关影响。 */
    std::function<void(void)> on_opus_packet_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
    // Called after the first streamed-player PCM block is handed to the codec.
    std::function<void(const PlaybackOutputEvent&)> on_player_output_started;
    std::function<void(const PlaybackOutputEvent&)> on_player_output_progress;
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToOpusQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
    kAudioTaskTypePlayerToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    /* Streamed-player PCM is explicitly allocated in PSRAM. Keeping a
       separate buffer leaves latency-sensitive capture/AEC/Opus PCM using
       its existing allocator and access pattern. */
    int16_t* player_pcm = nullptr;
    size_t player_pcm_samples = 0;
    uint32_t upload_generation = 0;
    PlaybackOutputEvent player_output;

    AudioTask() = default;
    ~AudioTask();
    AudioTask(const AudioTask&) = delete;
    AudioTask& operator=(const AudioTask&) = delete;

    bool AllocatePlayerPcm(size_t samples);
    int16_t* PlaybackData() {
        return player_pcm != nullptr ? player_pcm : pcm.data();
    }
    size_t PlaybackSamples() const {
        return player_pcm != nullptr ? player_pcm_samples : pcm.size();
    }
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
};

struct AudioUplinkDiagnostics {
    bool upload_enabled = false;
    bool processor_running = false;
    uint32_t generation = 0;
    size_t encode_queue = 0;
    size_t opus_queue = 0;
    uint32_t mic_reads = 0;
    uint32_t afe_frames = 0;
    uint32_t encode_dequeued = 0;
    uint32_t encoded_packets = 0;
    uint32_t send_notifications = 0;
    uint32_t popped_packets = 0;
    uint32_t dropped_encode_frames = 0;
    uint32_t last_mic_ms = 0;
    uint32_t last_afe_ms = 0;
    uint32_t last_encoded_ms = 0;
    uint32_t last_notification_ms = 0;
    uint32_t last_popped_ms = 0;
};

class AudioService {
public:
    AudioService() = default;
    ~AudioService() = default;

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    std::string GetWakeWordName() const;
    bool IsVoiceDetected() const { return voice_detected_.load(); }
    bool IsIdle();
    bool IsWakeWordRunning() const { return wake_word_running_.load(); }
    bool IsAfeWakeWord();

    void EnableWakeWordDetection(bool enable);
    void PauseWakeWordDetection();
    /* 只控制业务是否消费新 Opus，不启停采集、AFE/AEC 或编码任务。 */
    void EnableAudioUpload(bool enable);
    void EnableAudioTesting(bool enable);
    void SetAecEnabled(bool enable);
    bool SupportsAec() const;

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    bool PushPcmToPlaybackQueue(const uint8_t* data, size_t size, int sample_rate,
                                const PlaybackOutputEvent& output,
                                bool wait = true);
    void BeginPlayerPlayback(const PlaybackOutputEvent& output);
    void MarkPlayerPlaybackFinished(const PlaybackOutputEvent& output);
    void ClearPlaybackQueue();
    void WaitForPlaybackComplete();
    uint32_t GetPlayerOutputLastActivityMs() const {
        return player_output_last_activity_ms_.load();
    }
    std::unique_ptr<AudioStreamPacket> PopOpusPacket();
    AudioUplinkDiagnostics GetUplinkDiagnostics();
    void PlaySound(const std::string_view& sound);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    bool LoadWakeWordResources(bool force_reload = false);
#if CONFIG_USE_MICRO_WAKE_WORD
    void SetMicroWakeWordModel(const void* model_data, size_t model_size,
                               const std::string& wake_word = {},
                               const MicroWakeWordConfig& config = {});
#else // !CONFIG_USE_MICRO_WAKE_WORD
    void SetModelsList(srmodel_list_t* models_list);
#endif // CONFIG_USE_MICRO_WAKE_WORD

#ifdef CONFIG_LSPLATFORM
    void SetNarrowbandMode(bool enabled);
#endif // CONFIG_LSPLATFORM

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    std::string wake_word_name_;
    std::unique_ptr<AudioDebugger> audio_debugger_;
    std::mutex audio_debugger_mutex_;
    std::unique_ptr<OpusWbEncoder> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;
    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
#ifdef CONFIG_LSPLATFORM
    OpusResampler uplink_resampler_;
#endif // CONFIG_LSPLATFORM
    OpusResampler output_resampler_;
    /* TTS URL 播放器等外部 PCM 源的采样率与 codec 输出采样率可能不一致，由它按需重采样 */
    OpusResampler player_resampler_;
    int player_resampler_input_rate_ = 0; // 0 表示尚未配置
    std::mutex player_resampler_mutex_;
    DebugStatistics debug_statistics_;
    srmodel_list_t* models_list_ = nullptr;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    bool opus_codec_task_stack_external_ = false;
#if CONFIG_USE_MICRO_WAKE_WORD
    TaskHandle_t processed_wake_task_handle_ = nullptr;
    std::mutex processed_wake_mutex_;
    std::condition_variable processed_wake_cv_;
    std::deque<std::vector<int16_t>> processed_wake_queue_;
    std::atomic_bool wake_uses_processor_output_{false};
#endif
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    /* 固定长度的最新 Opus 窗口；生产者永不等待业务消费者。 */
    std::deque<std::unique_ptr<AudioStreamPacket>> opus_packet_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    bool audio_output_busy_ = false;
    bool player_playback_finished_ = false;
    bool player_tts_stream_ = false;
    bool player_prebuffering_ = true;
    bool player_prebuffer_wait_logged_ = false;
    uint32_t player_rebuffer_count_ = 0;
    size_t player_pcm_bytes_ = 0;
    /* Diagnostics for streamed-player jitter. These are written by the
       output task and reset when a new player session starts. */
    std::atomic<uint32_t> player_output_chunks_{0};
    std::atomic<uint32_t> player_output_last_end_ms_{0};
    std::atomic<uint32_t> player_output_last_activity_ms_{0};
    std::atomic<uint32_t> player_output_underruns_{0};
    std::atomic<uint32_t> player_audio_clock_ms_{0};
    std::atomic_bool player_stream_active_{false};
    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    std::atomic_bool voice_detected_{false};
    std::atomic_bool wake_word_running_{false};
    std::atomic_bool audio_testing_running_{false};
    std::atomic_bool service_stopped_{true};
    uint32_t upload_generation_ = 0;
    bool audio_upload_enabled_ = false;
    bool aec_enabled_ = false;

    // Monotonic stage counters make it possible to locate an uplink stall
    // without logging every 20 ms audio frame.
    std::atomic<uint32_t> uplink_mic_reads_{0};
    std::atomic<uint32_t> uplink_afe_frames_{0};
    std::atomic<uint32_t> uplink_encode_dequeued_{0};
    std::atomic<uint32_t> uplink_encoded_packets_{0};
    std::atomic<uint32_t> uplink_send_notifications_{0};
    std::atomic<uint32_t> uplink_popped_packets_{0};
    std::atomic<uint32_t> uplink_dropped_encode_frames_{0};
    std::atomic<uint32_t> uplink_last_mic_ms_{0};
    std::atomic<uint32_t> uplink_last_afe_ms_{0};
    std::atomic<uint32_t> uplink_last_encoded_ms_{0};
    std::atomic<uint32_t> uplink_last_notification_ms_{0};
    std::atomic<uint32_t> uplink_last_popped_ms_{0};

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
#if CONFIG_USE_MICRO_WAKE_WORD
    void ProcessedWakeTask();
    void SetWakeInputFromProcessor(bool enable);
    void ClearProcessedWakeQueue();
#endif
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
#ifdef CONFIG_LSPLATFORM
    void SetEncodeSampleRate(int sample_rate, int frame_duration);
#endif // CONFIG_LSPLATFORM
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
};

#endif
