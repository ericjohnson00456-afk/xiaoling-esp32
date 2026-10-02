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

#include "codecs/audio_codec.h"
#include "processors/audio_processor.h"
#include "opus_encoder/opus_wb_encoder.h"
#include "player/types/playback_event.h"
#include "wake_words/wake_word.h"
#include "wake_words/micro_wake_word/config.h"
#include "xiaoling/xiaoling_types.h"


/*
 * AudioService 只维护两条固定数据链：
 * 1. ADC -> 16 kHz PCM -> AFE/AEC -> Opus 最新窗口
 * 2. 播放 PCM/下行 Opus -> PCM -> DAC
 *
 * Application 持有唯一实例。Initialize() 装配 codec 和算法，Start() 后
 * 采集、算法和 Opus 编码持续运行；OTA 可用 Stop()/Start() 暂停服务。
 * EnableAudioUpload() 只决定网络是否消费新 Opus。
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
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
};

// 上行 Opus 是实时数据通知，不经过 AppEvent，也不属于音频业务事件。
struct AudioServiceUploadCallbacks {
    /* 只在业务打开音频上报后通知；Opus 的生产本身不受此开关影响。 */
    std::function<void(void)> on_opus_packet_available;
};

// 播放输出通知是播放器和音频硬件之间的内部接口，不属于音频业务事件。
// AudioPlayer 负责维护播放通道，并将通知转换成 PlayerOutput* AppEvent。
struct AudioServiceOutputCallbacks {
    // 首个播放器 PCM 完成 codec 输出后触发，用于确认扬声器真正开始播报。
    std::function<void(const PlaybackOutputEvent&)> on_output_started;
    // 每个播放器 PCM 块输出后触发，AudioPlayer 负责合并高频进度通知。
    std::function<void(const PlaybackOutputEvent&)> on_output_progress;
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

class AudioService {
public:
    AudioService() = default;
    ~AudioService() = default;

    // ===== 1. 服务生命周期 =====
    // Initialize 装配一次；Start 创建服务任务；Stop 用于 OTA 等系统级暂停。
    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();

    // ===== 2. 硬件采集与 AEC/前处理 =====
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    bool SupportsAec() const;
    void SetAecEnabled(bool enable);
    bool IsVoiceDetected() const { return voice_detected_.load(); }

    // ===== 3. 唤醒词 =====
    // 唤醒词检测结果由 Application 回调发布 AppEvent。
    void EnableWakeWordDetection(bool enable);
    void PauseWakeWordDetection();
    bool IsWakeWordRunning() const { return wake_word_running_.load(); }
    const std::string& GetLastWakeWord() const;
    std::string GetWakeWordName() const;
    bool IsAfeWakeWord();

    // 读取当前资源分区中的唤醒模型；失败只影响唤醒词，不阻断音频服务。
    bool LoadWakeWordResources();
#if CONFIG_USE_MICRO_WAKE_WORD
    void SetMicroWakeWordModel(const void* model_data, size_t model_size,
                               const std::string& wake_word = {},
                               const MicroWakeWordConfig& config = {});
#else // !CONFIG_USE_MICRO_WAKE_WORD
    void SetModelsList(srmodel_list_t* models_list);
#endif // CONFIG_USE_MICRO_WAKE_WORD

    // ===== 4. 上行 Opus 与录音测试 =====
    /* 只控制业务是否消费新 Opus，不启停采集、AFE/AEC 或编码任务。 */
    void EnableAudioUpload(bool enable);
    std::unique_ptr<AudioStreamPacket> PopOpusPacket();
    void EnableAudioTesting(bool enable);
#ifdef CONFIG_LSPLATFORM
    void SetNarrowbandMode(bool enabled);
#endif // CONFIG_LSPLATFORM

    // ===== 5. 下行解码与播放 =====
    // 服务端 Opus 走解码队列，本地播放器 PCM 直接走播放队列。
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    bool PushPcmToPlaybackQueue(const uint8_t* data, size_t size, int sample_rate,
                                const PlaybackOutputEvent& output,
                                bool wait = true);
    void BeginPlayerPlayback(const PlaybackOutputEvent& output);
    // 暂存音乐暂停瞬间已经解码、但尚未送到 DAC 的 PCM，避免恢复时跳过。
    void SuspendPlayerPlayback();
    void ResumePlayerPlayback(const PlaybackOutputEvent& output);
    void MarkPlayerPlaybackFinished(const PlaybackOutputEvent& output);
    // 清理当前活动播放队列，但保留被音乐暂停保存的 PCM。
    void ClearActivePlaybackQueue();
    void ClearPlaybackQueue();
    void WaitForPlaybackComplete();
    uint32_t GetPlayerOutputLastActivityMs() const {
        return player_output_last_activity_ms_.load();
    }
    void PlaySound(const std::string_view& sound);
    void ResetDecoder();
    // IsIdle 只检查工作队列是否为空，不代表常驻采集任务已停止。
    bool IsIdle();

    // ===== 6. 回调与外部状态 =====
    // 回调在音频工作任务上下文触发，Application 负责转换成业务事件。
    void SetCallbacks(AudioServiceCallbacks& callbacks);
    // 网络层注册 Opus 数据就绪通知；该通知不经过 AppEvent。
    void SetUploadCallbacks(AudioServiceUploadCallbacks& callbacks);
    // 播放器注册底层输出通知；AudioService 不直接发布播放器 AppEvent。
    void SetOutputCallbacks(AudioServiceOutputCallbacks& callbacks);

private:
    // ===== 1. 硬件和算法对象 =====
    // codec_ 归 Board 所有；本类只持有借用指针。
    AudioCodec* codec_ = nullptr;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    std::string wake_word_name_;

    // ===== 2. 唤醒模型资源与处理后音频旁路 =====
    // 唤醒模型列表传给 AFE/WakeWord；具体来源由资源装载路径决定。
    srmodel_list_t* models_list_ = nullptr;
#if CONFIG_USE_MICRO_WAKE_WORD
    // AEC 输出到唤醒推理任务的短队列；切输入源时清空。
    TaskHandle_t processed_wake_task_handle_ = nullptr;
    std::mutex processed_wake_mutex_;
    std::condition_variable processed_wake_cv_;
    std::deque<std::vector<int16_t>> processed_wake_queue_;
    // true 时 MicroWakeWord 消费 AEC 输出，false 时采集任务直接喂麦克风 PCM。
    std::atomic_bool wake_uses_processor_output_{false};
#endif

    // ===== 3. Opus 编解码器与重采样器 =====
    // Opus 编码用于上行，解码用于下行。
    std::unique_ptr<OpusWbEncoder> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;
    // ADC 麦克风与参考声道分别重采样到 16 kHz，保留声道顺序供 AEC 使用。
    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
#ifdef CONFIG_LSPLATFORM
    // 小聆窄带上报时，编码前再由 16 kHz 转为 8 kHz。
    OpusResampler uplink_resampler_;
#endif // CONFIG_LSPLATFORM
    // 下行 Opus 解码后转换为 codec 的 DAC 采样率。
    OpusResampler output_resampler_;
    /* TTS URL 播放器等外部 PCM 源的采样率与 codec 输出采样率可能不一致，由它按需重采样 */
    OpusResampler player_resampler_;
    int player_resampler_input_rate_ = 0; // 0 表示尚未配置
    std::mutex player_resampler_mutex_;

    // ===== 4. 服务任务 =====
    // Start() 创建的服务任务：ADC 采集、DAC 播放、上下行 Opus 共用任务。
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    // Opus 任务优先使用 PSRAM 栈；退出时选用匹配的 FreeRTOS 删除函数。
    bool opus_codec_task_stack_external_ = false;
    // ===== 5. 音频队列 =====
    // 共用 mutex/cv 保护以下编码、解码和播放队列及非原子状态。
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    // 服务端 Opus 等待解码；编码后的上行 Opus 保留最新窗口；测试包用于回放。
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    /* 固定长度的最新 Opus 窗口；生产者永不等待业务消费者。 */
    std::deque<std::unique_ptr<AudioStreamPacket>> opus_packet_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    // AFE 输出的 PCM 待编码；下行解码或播放器 PCM 待送 DAC。
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    // 音乐被 TTS/提示音抢占时保存的待输出 PCM；恢复后重新放回活动队列。
    std::deque<std::unique_ptr<AudioTask>> suspended_player_playback_queue_;
    size_t suspended_player_pcm_bytes_ = 0;

    // ===== 6. 播放状态 =====
    // 播放器流的预缓冲、尾帧和等待状态，由 audio_queue_mutex_ 保护。
    bool audio_output_busy_ = false;
    // true 表示播放器不再入新帧，允许 AudioOutputTask 播放不足预缓冲长度的尾音。
    bool player_playback_finished_ = false;
    // TTS 首播使用更大的 PCM 预缓冲门槛。
    bool player_tts_stream_ = false;
    bool player_prebuffering_ = true;
    bool player_prebuffer_wait_logged_ = false;
    uint32_t player_rebuffer_count_ = 0;
    // 本次播放器会话已入队的 PCM 字节数，用于尾帧 DMA 对齐。
    size_t player_pcm_bytes_ = 0;
    // 播放器首帧、输出活动和累计播放时钟，供播放控制与进度事件使用。
    std::atomic<uint32_t> player_output_chunks_{0};
    std::atomic<uint32_t> player_output_last_activity_ms_{0};
    std::atomic<uint32_t> player_audio_clock_ms_{0};
    std::atomic_bool player_stream_active_{false};

    // ===== 7. 运行开关与会话状态 =====
    // 算法初始化标志；running 表示当前喂帧，非任务是否存在。
    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    std::atomic_bool voice_detected_{false};
    std::atomic_bool wake_word_running_{false};
    std::atomic_bool audio_testing_running_{false};
    // Start/Stop 控制采集、播放、Opus 服务循环的退出条件。
    std::atomic_bool service_stopped_{true};
    // 硬件支持时默认打开，可由会话策略切换。
    bool aec_enabled_ = false;

    // ===== 8. 回调、上报和电源状态 =====
    AudioServiceCallbacks callbacks_;
    AudioServiceUploadCallbacks upload_callbacks_;
    AudioServiceOutputCallbacks output_callbacks_;
    // 上报会话代数用于剔除跨会话编码帧；业务开关不控制编码任务。
    uint32_t upload_generation_ = 0;
    bool audio_upload_enabled_ = false;
    // codec 空闲关电计时；新读写会按需重新打开对应方向。
    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    // ===== 9. 私有任务与队列辅助函数 =====
    // 工作任务及队列/采样率辅助函数，数据流见文件顶部总览。
    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    void ResetPlayerPlaybackState(bool clear_suspended_queue);
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
