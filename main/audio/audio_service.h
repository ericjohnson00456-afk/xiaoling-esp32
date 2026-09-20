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
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <model_path.h>

#include <opus_decoder.h>
#include <opus_resampler.h>

#include "audio_codec.h"
#include "audio_processor.h"
#include "opus_encoder/opus_wb_encoder.h"
#include "processors/audio_debugger.h"
#include "wake_word.h"
#include "wake_words/micro_wake_word/config.h"
#include "xiaoling/xiaoling_types.h"


/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Processors] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use one task for MIC / Speaker / Processors, and one task for Opus Encoder / Opus Decoder.
 * 
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM packets.
 * 
 */

// lschat 的 opus-wb 使用 20 ms 分包，每包包含 320 个 16 kHz 样本。
#define OPUS_FRAME_DURATION_MS 20
#define MAX_ENCODE_TASKS_IN_QUEUE 2
/* Keep the normal server-downlink queue small so interactive Opus playback
 * remains responsive. Network TTS uses a separate, deeper limit below. */
#define MAX_PLAYBACK_TASKS_IN_QUEUE 8
/* The TTS endpoint can pause for several seconds between chunks. A decoded
 * PCM block is about 36 ms at 16 kHz; 96 blocks provide about 3.5 s of
 * startup buffering without making the first response feel sluggish. */
#define MAX_PLAYER_PLAYBACK_TASKS_IN_QUEUE 160
#define PLAYER_PREBUFFER_TASKS 96
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_PROCESSED_WAKE_FRAMES_IN_QUEUE 4
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000


#define AS_EVENT_AUDIO_TESTING_RUNNING      (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1 << 3)

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
    // Called after the first streamed-player PCM block is handed to the codec.
    std::function<void(void)> on_player_output_started;
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
    kAudioTaskTypePlayerToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp = 0;
    uint32_t uplink_generation = 0;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    std::string GetWakeWordName() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    bool IsIdle();
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }
    bool IsAfeWakeWord();

    void EnableWakeWordDetection(bool enable);
    void PauseWakeWordDetection();
    void EnableAudioUpload(bool enable);
    void EnableVoiceProcessing(bool enable);
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);
    bool SupportsDeviceAec() const;
    bool SupportsAec() const;

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    bool PushPcmToPlaybackQueue(const uint8_t* data, size_t size, int sample_rate, bool wait = true);
    void MarkPlayerPlaybackFinished();
    void ClearPlaybackQueue();
    void WaitForPlaybackComplete();
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    bool HasPendingSendPackets();
    void PlaySound(const std::string_view& sound);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
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

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
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
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    bool audio_output_busy_ = false;
    bool player_playback_finished_ = false;
    bool player_prebuffer_wait_logged_ = false;
    bool player_prebuffer_release_logged_ = false;
    /* Diagnostics for streamed-player jitter. These are written by the
       output task and reset when a new player session starts. */
    std::atomic<uint32_t> player_output_chunks_{0};
    std::atomic<uint32_t> player_output_last_end_ms_{0};
    std::atomic<uint32_t> player_output_underruns_{0};
    // For server AEC
    std::deque<uint32_t> timestamp_queue_;

    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    bool voice_detected_ = false;
    bool service_stopped_ = true;
    bool audio_input_need_warmup_ = false;
    uint32_t uplink_generation_ = 0;
    bool uplink_enabled_ = false;
    bool device_aec_requested_ = false;

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
