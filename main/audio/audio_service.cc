#include "audio_service.h"
#include "playback_session_guard.h"
#include "playback_stream_policy.h"
#include "audio_yield_policy.h"
#include "input_discard_policy.h"
#include "resource/resource.h"
#include "xiaoling/conn/xiaoling_config.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <cJSON.h>

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_USE_MICRO_WAKE_WORD
#include "wake_words/micro_wake_word.h"
#include "wake_words/micro_wake_word/model_wake_word.h"
#else // CONFIG_USE_MICRO_WAKE_WORD
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"
#else
#include "wake_words/esp_wake_word.h"
#endif
#endif // CONFIG_USE_MICRO_WAKE_WORD

#define TAG "AudioService"

/*
 * 阅读顺序：Initialize/Start -> AudioInputTask -> AudioProcessor::OnOutput
 * -> PushTaskToEncodeQueue -> OpusCodecTask -> PopOpusPacket。
 *
 * 上行：ADC -> ReadAudioData(16 kHz，保留参考声道) -> AFE/AEC 或直通
 *       -> 20 ms PCM 编码队列 -> Opus 最新窗口 -> 业务按需取包。
 * 唤醒：从麦克风 PCM 或 AEC 输出旁路取帧 -> WakeWord -> 回调；
 *       Application 的回调负责将检测结果发布为 AppEvent。
 * 下行：服务端 Opus -> 同一个 OpusCodecTask 解码，或播放器直接送 PCM
 *       -> 播放队列 -> 唯一的 AudioOutputTask -> DAC。
 *
 * Application 持有 AudioService 一份实例。正常运行时任务常驻；唤醒检测、
 * 网络上报和 AEC 模式可切换。OTA 会调用 Stop()/Start() 暂停并重建服务任务。
 */
namespace {
constexpr UBaseType_t kAudioAlgorithmPriority = 8;
constexpr UBaseType_t kAudioCodecPriority = kAudioAlgorithmPriority;
#if CONFIG_USE_AUDIO_PROCESSOR
constexpr UBaseType_t kAudioAfePriority = AfeAudioProcessor::kTaskPriority;
#else
constexpr UBaseType_t kAudioAfePriority = kAudioAlgorithmPriority;
#endif
// I2S 无积压时会阻塞采集任务；有积压时仍需定期让 CPU0 Idle 获得调度。
// 不能每个 10 ms 音频帧都额外等待一 tick（当前也是 10 ms），否则 DSP
// 开销会使采集持续落后。100 ms 才主动让步，短暂延迟由 DMA 环覆盖。
constexpr int64_t kAudioInputYieldIntervalUs = 100 * 1000;
constexpr TickType_t kAudioInputYieldTicks = 1;

#if CONFIG_USE_MICRO_WAKE_WORD
// 唤醒推理与 ADC feed 分核；高于 AFE/编码、本地提示音与网络播放器。
// IDF 浮点任务会自动绑定核心，因此不依赖 NO_AFFINITY 进行负载均衡。
constexpr BaseType_t kProcessedWakeTaskCore = 1;
constexpr UBaseType_t kAudioWakePriority = kAudioAlgorithmPriority + 1;
constexpr int64_t kWakeYieldIntervalUs = 50 * 1000;
#endif

#if CONFIG_USE_MICRO_WAKE_WORD
bool HasSuffix(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ModelPathForDescriptor(const std::string& descriptor_path) {
    if (!HasSuffix(descriptor_path, ".json")) {
        return {};
    }
    return descriptor_path.substr(0, descriptor_path.size() - 5) + ".tflite";
}

bool ParseMicroWakeWordConfig(const cJSON* descriptor, MicroWakeWordConfig& config) {
    const cJSON* micro = cJSON_GetObjectItemCaseSensitive(descriptor, "micro");
    if (!cJSON_IsObject(micro)) {
        return false;
    }
    const cJSON* cutoff = cJSON_GetObjectItemCaseSensitive(micro, "probability_cutoff");
    if (!cJSON_IsNumber(cutoff) || cutoff->valuedouble <= 0.0 ||
        cutoff->valuedouble > 1.0) {
        return false;
    }
    config.probability_cutoff = static_cast<float>(cutoff->valuedouble);
    const cJSON* sliding = cJSON_GetObjectItemCaseSensitive(micro, "sliding_window_size");
    if (cJSON_IsNumber(sliding) && sliding->valuedouble >= 1.0 && sliding->valuedouble <= 64.0) {
        config.sliding_window_size = static_cast<size_t>(sliding->valuedouble);
    }
    const cJSON* step = cJSON_GetObjectItemCaseSensitive(micro, "feature_step_size");
    if (cJSON_IsNumber(step) && step->valuedouble >= 1.0 && step->valuedouble <= 100.0) {
        config.feature_step_size = static_cast<uint8_t>(step->valuedouble);
    }
    const cJSON* arena = cJSON_GetObjectItemCaseSensitive(micro, "tensor_arena_size");
    if (cJSON_IsNumber(arena) && arena->valuedouble >= 16.0 * 1024.0 &&
        arena->valuedouble <= 1024.0 * 1024.0) {
        config.tensor_arena_size = static_cast<size_t>(arena->valuedouble);
    }
    return true;
}
#endif
}  // namespace

AudioTask::~AudioTask() {
    if (player_pcm != nullptr) {
        heap_caps_free(player_pcm);
    }
}

bool AudioTask::AllocatePlayerPcm(size_t samples) {
    if (player_pcm != nullptr) {
        heap_caps_free(player_pcm);
        player_pcm = nullptr;
        player_pcm_samples = 0;
    }
    if (samples == 0) {
        return true;
    }

    player_pcm = static_cast<int16_t*>(heap_caps_aligned_alloc(
        16, samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (player_pcm == nullptr) {
        return false;
    }
    player_pcm_samples = samples;
    return true;
}


// ============================================================================
// 1. 服务生命周期与初始化
// ============================================================================
// 启动 codec 后先加载唤醒资源，再装配 Opus 编解码器和音频处理器。
void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    // ESP-SR 会把模型列表交给 AFE；MicroWakeWord 则只创建独立的唤醒对象。
    if (!LoadWakeWordResources()) {
        ESP_LOGW(TAG, "Failed to configure wake-word model from resource table");
    }

    /* Setup the audio codec */
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusWbEncoder>(16000, 1, OPUS_FRAME_DURATION_MS);
    audio_testing_encoder_ =
        std::make_unique<OpusWbEncoder>(16000, 1, OPUS_FRAME_DURATION_MS);

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    // 处理器（AFE/AEC 或直通）每输出一帧，可选地复制给唤醒旁路，原帧交给编码。
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
#if CONFIG_USE_MICRO_WAKE_WORD
        const uint32_t wake_generation = wake_input_generation_.load();
#endif
        // 测试模式从云端编码器的同一输入点旁路取样，保证 A/B 对比包含
        // AFE/AEC 的影响，而不是录到空闲时的原始麦克风旁路。
        CaptureUplinkPcmForTesting(data);
#if CONFIG_USE_MICRO_WAKE_WORD
        if (IsWakeWordRunning()) {
            QueueWakeFrame(data, wake_generation);
        }
#endif
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToOpusQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    /*
     * 音频处理链只初始化一次。ADC -> AFE/AEC 常驻；上报开启才编码 Opus。
     * 业务层不再负责反复创建或停止算法任务。
     */
    audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
    audio_processor_initialized_ = true;

    // AEC 是 AudioService 自身的硬件能力：板型和 codec 都支持时默认开启。
    aec_enabled_ = SupportsAec();
    ESP_LOGI(TAG, "AEC: supported=%d, default=%s",
             SupportsAec() ? 1 : 0, aec_enabled_ ? "on" : "off");

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);

}

// 启动服务循环；AFE 内部任务已在 Initialize() 创建，Start() 使其开始取帧。
void AudioService::Start() {
    if (!service_stopped_.exchange(false)) {
        ESP_LOGW(TAG, "音频服务已经启动，忽略重复 Start()");
        return;
    }
    audio_testing_running_.store(false);
    wake_word_running_.store(false);
    local_encoding_benchmark_.store(false);

    audio_processor_->SetAecEnabled(aec_enabled_);
    audio_processor_->Start();
#if CONFIG_USE_MICRO_WAKE_WORD
    ClearProcessedWakeQueue();
    ESP_LOGI(TAG, "Wake word input source: processed PCM (BOX3: AEC -> NS -> AGC), always");
#endif

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, kAudioAlgorithmPriority,
       &audio_input_task_handle_, 0);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, kAudioCodecPriority,
       &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, kAudioAlgorithmPriority,
       &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, kAudioCodecPriority,
       &audio_output_task_handle_);
#endif

    /* Opus needs a large stack, but its working set does not need scarce
       internal SRAM. Keep that SRAM available for Wi-Fi RX and DMA. */
    auto opus_task = [](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        const bool external_stack = audio_service->opus_codec_task_stack_external_;
        audio_service->opus_codec_task_handle_ = nullptr;
        if (external_stack) {
            vTaskDeleteWithCaps(nullptr);
        } else {
            vTaskDelete(nullptr);
        }
    };
    opus_codec_task_stack_external_ = true;
    BaseType_t opus_task_result = xTaskCreateWithCaps(
        opus_task, "opus_codec", 2048 * 13, this, kAudioCodecPriority,
        &opus_codec_task_handle_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (opus_task_result != pdPASS) {
        opus_codec_task_stack_external_ = false;
        opus_task_result = xTaskCreate(
            opus_task, "opus_codec", 2048 * 13, this, kAudioCodecPriority,
            &opus_codec_task_handle_);
    }
    if (opus_task_result != pdPASS) {
        ESP_LOGE(TAG, "创建 Opus 编解码任务失败");
    } else {
        ESP_LOGI(TAG, "Opus 编解码任务栈: %s, bytes=%u",
                 opus_codec_task_stack_external_ ? "PSRAM" : "internal",
                 static_cast<unsigned>(2048 * 13));
    }

#if CONFIG_USE_MICRO_WAKE_WORD
    BaseType_t wake_task_result = xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->ProcessedWakeTask();
        vTaskDeleteWithCaps(nullptr);
    }, "processed_wake", 2048 * 3, this, kAudioWakePriority,
       &processed_wake_task_handle_, kProcessedWakeTaskCore,
       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (wake_task_result != pdPASS) {
        ESP_LOGW(TAG, "PSRAM 唤醒任务栈分配失败，回退内部 SRAM");
        xTaskCreatePinnedToCore([](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->ProcessedWakeTask();
            vTaskDelete(nullptr);
        }, "processed_wake", 2048 * 3, this, kAudioWakePriority,
           &processed_wake_task_handle_, kProcessedWakeTaskCore);
    }
#endif

    ESP_LOGI(TAG,
             "音频任务优先级: output=%u, opus=%u, input=%u, wake=%u, afe=%u",
             static_cast<unsigned>(kAudioCodecPriority),
             static_cast<unsigned>(kAudioCodecPriority),
             static_cast<unsigned>(kAudioAlgorithmPriority),
#if CONFIG_USE_MICRO_WAKE_WORD
             static_cast<unsigned>(kAudioWakePriority),
#else
             static_cast<unsigned>(kAudioAlgorithmPriority),
#endif
             static_cast<unsigned>(kAudioAfePriority));
}

// 系统级暂停（目前由 OTA 使用）：通知任务退出并清空队列；普通业务切换不调用。
void AudioService::Stop() {
    if (service_stopped_.exchange(true)) {
        return;
    }
    esp_timer_stop(audio_power_timer_);
    local_encoding_benchmark_.store(false);
    audio_testing_running_.store(false);
    // A/B 编解码任务会使用测试 PCM 和播放队列；系统停止前先让它安全退出，
    // 避免 OTA 清队列时与其并发写入。
    while (audio_testing_processing_.load() &&
           xTaskGetCurrentTaskHandle() != audio_testing_task_handle_) {
        vTaskDelay(1);
    }
    wake_word_running_.store(false);
    if (audio_processor_initialized_) {
        audio_processor_->Stop();
    }
    {
        std::lock_guard<std::mutex> testing_lock(audio_testing_pcm_mutex_);
        audio_testing_pcm_buffer_.clear();
    }
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_upload_enabled_ = false;
    ++upload_generation_;
    audio_encode_queue_.clear();
    opus_packet_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    suspended_player_playback_queue_.clear();
    suspended_player_pcm_bytes_ = 0;
    audio_queue_cv_.notify_all();
#if CONFIG_USE_MICRO_WAKE_WORD
    ClearProcessedWakeQueue();
    processed_wake_cv_.notify_all();
#endif
}

// ============================================================================
// 2. 硬件采集与 AEC/前处理
// ============================================================================
// 从 ADC 读指定时长的 PCM；必要时逐声道重采样，保留双声道中的参考声道。
// 增益只作用于第一个（麦克风）声道，返回的数据供唤醒与处理链共用。
bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (codec_->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    // 同期统计 ADC 原始 MIC/REF，区分采集噪声与增益、AFE 处理的影响。
    if (audio_testing_running_.load()) {
        std::lock_guard<std::mutex> lock(audio_testing_pcm_mutex_);
        if (audio_testing_running_.load()) {
            auto update = [](TestingInputStats& stats, int16_t sample) {
                constexpr size_t kMaxSamples =
                    AUDIO_TESTING_MAX_DURATION_MS * 16000 / 1000;
                if (stats.samples >= kMaxSamples) return;
                const int32_t value = sample;
                ++stats.samples;
                stats.peak = std::max(stats.peak, value < 0 ? -value : value);
                stats.sum += value;
                stats.square_sum += static_cast<uint64_t>(value * value);
            };
            const size_t channels = codec_->input_channels();
            for (size_t i = 0; i < data.size(); i += channels) {
                update(audio_testing_mic_stats_, data[i]);
                if (codec_->input_reference() && channels > 1) {
                    update(audio_testing_ref_stats_, data[i + channels - 1]);
                }
            }
        }
    }

    // Amplify the microphone before it is split to the wake-word and AFE pipelines.
#if CONFIG_BOARD_TYPE_ESP_BOX_3
    // BOX3 只在 AEC 前放大 MIC，参考声道保持原幅度。
    constexpr int32_t kMicPcmGain = 2;
#else
    constexpr int32_t kMicPcmGain = 5;
#endif
    const size_t input_channels = codec_->input_channels();
    size_t clipped = 0;
    for (size_t i = 0; i < data.size(); i += input_channels) {
        const int32_t amplified = static_cast<int32_t>(data[i]) * kMicPcmGain;
        if (amplified < INT16_MIN || amplified > INT16_MAX) ++clipped;
        data[i] = static_cast<int16_t>(std::clamp(amplified,
            static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
    }
#if CONFIG_BOARD_TYPE_ESP_BOX_3
    if (clipped > 0) {
        static uint32_t clipping_frames = 0;
        if (++clipping_frames % 100 == 1) {
            ESP_LOGW(TAG, "BOX3 AEC 前 x2 削波: %u/%u samples, frames=%lu",
                     static_cast<unsigned>(clipped),
                     static_cast<unsigned>(data.size() / input_channels),
                     static_cast<unsigned long>(clipping_frames));
        }
    }
#endif

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    return true;
}

// 常驻采集入口：取各消费者所需的最小帧长读 ADC，再分别拼成测试、AFE、唤醒帧。
// 处理器总是接收输入；wake_running 只决定是否向唤醒算法喂帧。
void AudioService::AudioInputTask() {
#if !CONFIG_USE_MICRO_WAKE_WORD
    std::vector<int16_t> wake_pending;
#endif
    std::vector<int16_t> processor_pending;
    int64_t last_yield_us = esp_timer_get_time();
    while (!service_stopped_.load()) {
        if (esp_timer_get_time() - last_yield_us >= kAudioInputYieldIntervalUs) {
            vTaskDelay(kAudioInputYieldTicks);
            // 不追赶已经错过的让步时间，避免负载高时反复睡眠无法清空 DMA。
            last_yield_us = esp_timer_get_time();
        }

#if CONFIG_USE_MICRO_WAKE_WORD
        // MicroWakeWord 始终从处理器输出取样；这里仅采集/喂 AFE。
        constexpr size_t wake_samples = 0;
#else
        const bool wake_running = wake_word_running_.load();
        if (!wake_running) wake_pending.clear();
        const size_t wake_samples = wake_running ? wake_word_->GetFeedSize() : 0;
#endif
        const size_t processor_samples = audio_processor_->GetFeedSize();
        size_t samples = 0;
        for (const size_t feed_size : {wake_samples, processor_samples}) {
            if (feed_size > 0 && (samples == 0 || feed_size < samples)) samples = feed_size;
        }
#if CONFIG_BOARD_TYPE_ESP_BOX_3
        // BOX3 的 DMA 每块 240 点，而 AFE 需要 256 点。IDF 在 RX 队列
        // 接近满时会跳过当前 DMA 块未读完的尾部，即使没有队列溢出事件。
        // 整块读取保证没有未读尾部；下面的 pending 再拼成 AFE 所需帧长。
        if (samples > 0 && codec_->input_sample_rate() == 16000) {
            samples = AUDIO_CODEC_DMA_FRAME_NUM;
        }
#endif
        if (samples == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const uint32_t discard_before = input_discard_state_.load();
        std::vector<int16_t> data;
        if (!ReadAudioData(data, 16000, samples)) {
            continue;
        }
        if (DiscardCapturedInput(data, discard_before, input_discard_state_.load())) {
            processor_pending.clear();
#if !CONFIG_USE_MICRO_WAKE_WORD
            wake_pending.clear();
#endif
        }

        const size_t channels = codec_->input_channels();
        if (processor_samples > 0) {
            processor_pending.insert(processor_pending.end(), data.begin(), data.end());
            const size_t processor_frame_size = processor_samples * channels;
            while (processor_pending.size() >= processor_frame_size &&
                   !service_stopped_.load()) {
                std::vector<int16_t> frame(processor_pending.begin(),
                                           processor_pending.begin() + processor_frame_size);
                processor_pending.erase(processor_pending.begin(),
                                        processor_pending.begin() + processor_frame_size);
                audio_processor_->Feed(std::move(frame));
            }
        }

#if !CONFIG_USE_MICRO_WAKE_WORD
        if (wake_samples > 0 && IsWakeWordRunning()) {
            wake_pending.insert(wake_pending.end(), data.begin(), data.end());
            const size_t wake_channels = channels;
            const size_t wake_frame_size = wake_samples * wake_channels;
            while (wake_pending.size() >= wake_frame_size && IsWakeWordRunning()) {
                std::vector<int16_t> frame(wake_pending.begin(),
                                           wake_pending.begin() + wake_frame_size);
                wake_pending.erase(wake_pending.begin(),
                                   wake_pending.begin() + wake_frame_size);
                wake_word_->Feed(frame);
            }
        }
#endif
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

// 测试录音从真正送入云端 Opus 编码器的 AFE/AEC PCM 旁路取样。
// 达到上限后保持测试状态，等待用户再次按键再开始两段对比回放。
void AudioService::CaptureUplinkPcmForTesting(const std::vector<int16_t>& data) {
    if (!audio_testing_running_.load() || data.empty()) {
        return;
    }

    constexpr size_t kMaxTestingSamples =
        AUDIO_TESTING_MAX_DURATION_MS * 16000 / 1000;
    bool reached_limit = false;
    {
        std::lock_guard<std::mutex> lock(audio_testing_pcm_mutex_);
        if (!audio_testing_running_.load()) {
            return;
        }
        const size_t remaining = kMaxTestingSamples -
                                 audio_testing_pcm_buffer_.size();
        const size_t copy_samples = std::min(remaining, data.size());
        if (copy_samples > 0) {
            audio_testing_pcm_buffer_.insert(
                audio_testing_pcm_buffer_.end(), data.begin(),
                data.begin() + copy_samples);
            reached_limit =
                audio_testing_pcm_buffer_.size() >= kMaxTestingSamples;
        }
    }
    if (reached_limit) {
        ESP_LOGI(TAG, "Audio testing reached %d ms; waiting for stop to compare",
                 AUDIO_TESTING_MAX_DURATION_MS);
    }
}

// ============================================================================
// 3. 唤醒词输入链
// ============================================================================
#if CONFIG_USE_MICRO_WAKE_WORD
// 唤醒旁路的有界队列；推理慢时仅丢唤醒帧，不能拖慢 ADC/AEC/云端录音。
void AudioService::QueueWakeFrame(std::vector<int16_t> data, uint32_t generation) {
    std::lock_guard<std::mutex> lock(processed_wake_mutex_);
    if (!IsWakeWordRunning() || service_stopped_.load() ||
        generation != wake_input_generation_.load()) {
        return;
    }
    if (processed_wake_queue_.size() >= MAX_PROCESSED_WAKE_FRAMES_IN_QUEUE) {
        processed_wake_queue_.pop_front();
        const uint32_t dropped_frames = wake_dropped_frames_.fetch_add(1) + 1;
        if ((dropped_frames % 100) == 1) {
            ESP_LOGW(TAG, "Wake queue full, dropped frames: %lu (ADC unaffected)",
                     static_cast<unsigned long>(dropped_frames));
        }
    }
    ++wake_produced_frames_;
    processed_wake_queue_.push_back({std::move(data), generation, esp_timer_get_time()});
    processed_wake_cv_.notify_one();
}

// 始终使用处理器输出，在 CPU1 独立推理；generation 排除启停前的旧帧。
void AudioService::ProcessedWakeTask() {
    int64_t last_yield_us = esp_timer_get_time();
    int64_t stats_started_us = last_yield_us;
    uint32_t previous_produced = wake_produced_frames_.load();
    uint32_t previous_dropped = wake_dropped_frames_.load();
    uint32_t processed = 0;
    int64_t inference_us = 0, max_inference_us = 0, max_age_us = 0;
    while (true) {
        if (esp_timer_get_time() - last_yield_us >= kWakeYieldIntervalUs) {
            vTaskDelay(kAudioInputYieldTicks);
            last_yield_us = esp_timer_get_time();
        }
        std::unique_lock<std::mutex> lock(processed_wake_mutex_);
        processed_wake_cv_.wait(lock, [this]() {
            return service_stopped_ || !processed_wake_queue_.empty();
        });
        if (service_stopped_) {
            break;
        }

        auto data = std::move(processed_wake_queue_.front());
        processed_wake_queue_.pop_front();
        lock.unlock();

        std::lock_guard<std::mutex> inference_lock(wake_inference_mutex_);
        if (!service_stopped_.load() && IsWakeWordRunning() &&
            data.generation == wake_input_generation_.load()) {
            const int64_t started_us = esp_timer_get_time();
            max_age_us = std::max(max_age_us, started_us - data.enqueued_us);
            wake_word_->Feed(data.pcm);
            const int64_t elapsed_us = esp_timer_get_time() - started_us;
            inference_us += elapsed_us;
            max_inference_us = std::max(max_inference_us, elapsed_us);
            ++processed;
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us - stats_started_us >= 5 * 1000 * 1000) {
            const uint32_t produced = wake_produced_frames_.load();
            const uint32_t dropped = wake_dropped_frames_.load();
            const double seconds = (now_us - stats_started_us) / 1000000.0;
            ESP_LOGI(TAG, "唤醒吞吐: input=%.1ffps, infer=%.1ffps, dropped=%lu, "
                     "avg=%.1fms, max=%.1fms, age_max=%.1fms",
                     (produced - previous_produced) / seconds, processed / seconds,
                     static_cast<unsigned long>(dropped - previous_dropped),
                     processed ? inference_us / (processed * 1000.0) : 0.0,
                     max_inference_us / 1000.0, max_age_us / 1000.0);
            previous_produced = produced;
            previous_dropped = dropped;
            stats_started_us = now_us;
            processed = 0;
            inference_us = max_inference_us = max_age_us = 0;
        }
    }
    ESP_LOGW(TAG, "Processed wake task stopped");
}

void AudioService::ClearProcessedWakeQueue() {
    std::lock_guard<std::mutex> lock(processed_wake_mutex_);
    ++wake_input_generation_;
    processed_wake_queue_.clear();
}

#endif

// ============================================================================
// 4. 下行播放任务
// ============================================================================
// DAC 的唯一写入者。服务端解码 PCM 与本地播放器 PCM 在此串行输出。
void AudioService::AudioOutputTask() {
    int testing_previous_volume = -1;
    auto restore_testing_volume = [&]() {
        if (testing_previous_volume >= 0) {
            codec_->SetOutputVolumeTemporary(testing_previous_volume);
            ESP_LOGI(TAG, "音频测试回放结束，恢复音量: %d%%",
                     testing_previous_volume);
            testing_previous_volume = -1;
        }
    };
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        if (audio_playback_queue_.empty() && player_stream_active_.load() &&
            !player_playback_finished_ && player_output_chunks_.load() != 0 &&
            player_underrun_grace_ms_ != 0) {
            const auto session = player_session_id_;
            const auto grace_ms = player_underrun_grace_ms_;
            const bool supplied = audio_queue_cv_.wait_for(lock,
                std::chrono::milliseconds(grace_ms), [this, session]() {
                    return service_stopped_ || !audio_playback_queue_.empty() ||
                           player_session_id_ != session || player_playback_finished_ ||
                           !player_stream_active_.load();
                });
            if (!supplied && ShouldRebufferPlayback(grace_ms, grace_ms,
                    audio_playback_queue_.empty(), player_session_id_ == session,
                    player_playback_finished_, player_output_chunks_.load() != 0)) {
                player_prebuffering_ = true;
                player_prebuffer_wait_logged_ = false;
                ++player_rebuffer_count_;
                ESP_LOGW(TAG, "播放器实际供给断流: session=%lu, waited=%lums, recovery=%lu",
                         static_cast<unsigned long>(session), static_cast<unsigned long>(grace_ms),
                         static_cast<unsigned long>(player_rebuffer_count_));
            }
        }
        audio_queue_cv_.wait(lock, [this, &testing_previous_volume]() {
            return !audio_playback_queue_.empty() || service_stopped_ ||
                   testing_previous_volume >= 0;
        });
        if (service_stopped_) {
            break;
        }
        if (testing_previous_volume >= 0 &&
            (audio_playback_queue_.empty() ||
             !audio_playback_queue_.front()->testing_playback)) {
            // 最后一块 PCM 被接收后，等待 I2S DMA 尾音实际播完再恢复。
            // 清队列取消测试也会进入这里，避免临时音量残留。
            const int sample_rate = codec_->output_sample_rate();
            lock.unlock();
            if (sample_rate > 0) {
                const int drain_ms =
                    (AUDIO_CODEC_DMA_DESC_NUM * AUDIO_CODEC_DMA_FRAME_NUM * 1000 +
                     sample_rate - 1) / sample_rate;
                vTaskDelay(pdMS_TO_TICKS(drain_ms));
            }
            restore_testing_volume();
            continue;
        }

        // TTS 用短缓冲和等待期限；音乐保留字节门槛。保存会话 ID，等待期间
        // 若 tone 接管输出，必须重新读取它的配置，不能继续等旧 TTS 水位。
        const bool is_player =
            audio_playback_queue_.front()->type == kAudioTaskTypePlayerToPlaybackQueue;
        const bool initial_prebuffer = player_output_chunks_.load() == 0;
        const uint32_t buffering_session = player_session_id_;
        const uint32_t max_wait_ms = player_prebuffer_max_wait_ms_;
        const auto buffer_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(max_wait_ms);
        const size_t prebuffer_target = initial_prebuffer
                                            ? player_initial_prebuffer_bytes_
                                            : player_recovery_prebuffer_bytes_;
        size_t prebuffered_pcm_bytes = is_player ? queued_player_pcm_bytes_ : 0;
        while (!service_stopped_ && !audio_playback_queue_.empty() &&
               audio_playback_queue_.front()->type == kAudioTaskTypePlayerToPlaybackQueue &&
               player_prebuffering_ &&
               prebuffered_pcm_bytes < prebuffer_target &&
               !player_playback_finished_ &&
               player_session_id_ == buffering_session &&
               is_player) {
            if (!player_prebuffer_wait_logged_) {
                player_prebuffer_wait_logged_ = true;
                ESP_LOGI(TAG, "播放器%s PCM 缓冲等待: bytes=%u/%u, queue=%u",
                         initial_prebuffer ? "首播" : "断流恢复",
                         static_cast<unsigned>(prebuffered_pcm_bytes),
                         static_cast<unsigned>(prebuffer_target),
                         static_cast<unsigned>(audio_playback_queue_.size()));
            }
            if (max_wait_ms != 0) {
                if (audio_queue_cv_.wait_until(lock, buffer_deadline) == std::cv_status::timeout) {
                    ESP_LOGI(TAG, "播放器缓冲到时先播: session=%lu, pcm=%u/%u, wait=%lums",
                             static_cast<unsigned long>(buffering_session),
                             static_cast<unsigned>(queued_player_pcm_bytes_),
                             static_cast<unsigned>(prebuffer_target),
                             static_cast<unsigned long>(max_wait_ms));
                    break;
                }
            } else {
                audio_queue_cv_.wait(lock);
            }
            prebuffered_pcm_bytes = audio_playback_queue_.empty()
                                         ? 0
                                         : queued_player_pcm_bytes_;
        }
        if (service_stopped_) {
            break;
        }
        if (audio_playback_queue_.empty()) {
            continue;
        }
        if (is_player && player_session_id_ != buffering_session) continue;
        if (is_player && player_prebuffering_) {
            player_prebuffering_ = false;
            player_prebuffer_wait_logged_ = false;
            ESP_LOGI(TAG,
                     "播放器缓冲释放: initial=%d, pcm_bytes=%u/%u, queue=%u, "
                     "stream_finished=%d, recovery=%lu",
                     initial_prebuffer ? 1 : 0,
                     static_cast<unsigned>(prebuffered_pcm_bytes),
                     static_cast<unsigned>(prebuffer_target),
                     static_cast<unsigned>(audio_playback_queue_.size()),
                     player_playback_finished_ ? 1 : 0,
                     static_cast<unsigned long>(player_rebuffer_count_));
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        if (task->type == kAudioTaskTypePlayerToPlaybackQueue) {
            const size_t task_bytes = task->PlaybackSamples() * sizeof(int16_t);
            queued_player_pcm_bytes_ = task_bytes <= queued_player_pcm_bytes_
                                           ? queued_player_pcm_bytes_ - task_bytes
                                           : 0;
        }
        if (task->type == kAudioTaskTypePlayerToPlaybackQueue &&
            audio_playback_queue_.empty() && !player_playback_finished_ &&
            player_underrun_grace_ms_ == 0) {
            player_prebuffering_ = true;
            player_prebuffer_wait_logged_ = false;
            ++player_rebuffer_count_;
            ESP_LOGW(TAG, "播放器队列欠载，等待下一块 PCM: recovery=%lu",
                     static_cast<unsigned long>(player_rebuffer_count_));
        }
        audio_output_busy_ = true;
        bool player_output_started = false;
        if (task->type == kAudioTaskTypePlayerToPlaybackQueue) {
            player_output_last_activity_ms_.store(esp_log_timestamp());
            player_output_started = player_output_chunks_.fetch_add(1) == 0;
        }
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }
        if (task->testing_playback && testing_previous_volume < 0) {
            testing_previous_volume = codec_->output_volume();
            codec_->SetOutputVolumeTemporary(100);
            ESP_LOGI(TAG, "音频测试临时音量: %d%% -> 100%% (不保存)",
                     testing_previous_volume);
        }
        codec_->OutputData(task->PlaybackData(), task->PlaybackSamples());
        lock.lock();
        const bool current_player_output = task->type == kAudioTaskTypePlayerToPlaybackQueue &&
                                          task->player_output.session_id == player_session_id_;
        const int sample_rate = codec_->output_sample_rate();
        const uint32_t duration_ms = sample_rate > 0
            ? static_cast<uint32_t>((static_cast<uint64_t>(task->PlaybackSamples()) * 1000) /
                                    static_cast<uint32_t>(sample_rate)) : 0;
        const uint32_t audio_clock_ms = current_player_output && duration_ms > 0
            ? player_audio_clock_ms_.fetch_add(duration_ms) + duration_ms : 0;
        if (current_player_output) player_output_last_activity_ms_.store(esp_log_timestamp());
        audio_output_busy_ = false;
        audio_queue_cv_.notify_all();
        lock.unlock();
        // codec 写入期间可能已切换会话，旧进度不能累加到新播放时钟。
        if (current_player_output) {
            if (player_output_started && output_callbacks_.on_output_started) {
                ESP_LOGI(TAG, "播放器首个 PCM 已交给 codec: session=%lu, samples=%u",
                         static_cast<unsigned long>(task->player_output.session_id),
                         static_cast<unsigned>(task->PlaybackSamples()));
                output_callbacks_.on_output_started(task->player_output);
            }
            if (duration_ms > 0) {
                if (output_callbacks_.on_output_progress) {
                    auto output = task->player_output;
                    output.audio_clock_ms = audio_clock_ms;
                    output_callbacks_.on_output_progress(output);
                }
            }
        }

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
    }

    restore_testing_volume();
    ESP_LOGW(TAG, "Audio output task stopped");
}

// ============================================================================
// 5. Opus 编解码任务与采样率配置
// ============================================================================
// 同一工作任务兼顾下行 Opus 解码和上行 PCM 编码；两侧共享队列锁。
// 上报关闭时不做无消费者的上行编码；下行解码不受影响。
void AudioService::OpusCodecTask() {
    bool encoder_has_generation = false;
    uint32_t encoder_generation = 0;
    int64_t stats_started_us = esp_timer_get_time();
    uint32_t previous_produced = encode_produced_frames_.load();
    uint32_t previous_dropped = encode_dropped_frames_.load();
    uint32_t encoded = 0;
    int64_t encoding_us = 0;
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                !audio_encode_queue_.empty() ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_->Decode(std::move(packet->payload), task->pcm)) {
                // Resample if the sample rate is different
                if (opus_decoder_->sample_rate() != codec_->output_sample_rate()) {
                    int target_size = output_resampler_.GetOutputSamples(task->pcm.size());
                    std::vector<int16_t> resampled(target_size);
                    output_resampler_.Process(task->pcm.data(), task->pcm.size(), resampled.data());
                    task->pcm = std::move(resampled);
                }

                lock.lock();
                audio_playback_queue_.push_back(std::move(task));
                audio_queue_cv_.notify_all();
            } else {
                ESP_LOGE(TAG, "Failed to decode audio");
                lock.lock();
            }
        }
        
        /* 编码不会等待业务消费者；Opus 队列满时在下方覆盖最旧包。 */
        if (!audio_encode_queue_.empty()) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            if (task->type == kAudioTaskTypeEncodeToOpusQueue &&
                ((!audio_upload_enabled_ && !local_encoding_benchmark_.load()) ||
                 task->upload_generation != upload_generation_)) {
                continue;
            }
            lock.unlock();

            // 编码器由此任务独占复位，防止停止/重开上报时携带旧会话状态。
            if (const auto ticks = AudioYieldDelayTicks(xTaskGetTickCount(), pdMS_TO_TICKS(100)); ticks) {
                vTaskDelay(ticks);
            }
            if (opus_encoder_ != nullptr &&
                (!encoder_has_generation || encoder_generation != task->upload_generation)) {
                opus_encoder_->ResetState();
                encoder_generation = task->upload_generation;
                encoder_has_generation = true;
            }
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            const int64_t encoding_started_us = esp_timer_get_time();
#ifdef CONFIG_LSPLATFORM
            const bool raw_uplink = task->type == kAudioTaskTypeEncodeToOpusQueue &&
                                    std::strcmp(XiaolingConfig::kUplinkAudioEncoding, "raw") == 0;
            if (raw_uplink) {
                // raw 协议要求 16 kHz、16-bit LE、单声道 PCM。
                packet->payload.resize(task->pcm.size() * sizeof(int16_t));
                for (size_t i = 0; i < task->pcm.size(); ++i) {
                    const auto sample = static_cast<uint16_t>(task->pcm[i]);
                    packet->payload[i * 2] = static_cast<uint8_t>(sample & 0xff);
                    packet->payload[i * 2 + 1] = static_cast<uint8_t>(sample >> 8);
                }
            }
            if (!raw_uplink &&
                (opus_encoder_ == nullptr ||
                 !opus_encoder_->Encode(std::move(task->pcm), packet->payload))) {
#else
            if (opus_encoder_ == nullptr ||
                !opus_encoder_->Encode(std::move(task->pcm), packet->payload)) {
#endif // CONFIG_LSPLATFORM
                ESP_LOGE(TAG, "Failed to encode audio");
                continue;
            }
            encoding_us += esp_timer_get_time() - encoding_started_us;
            ++encoded;
            const int64_t stats_now_us = esp_timer_get_time();
            if (stats_now_us - stats_started_us >= 5 * 1000 * 1000) {
                const uint32_t produced = encode_produced_frames_.load();
                const uint32_t dropped = encode_dropped_frames_.load();
                const double seconds = (stats_now_us - stats_started_us) / 1000000.0;
                ESP_LOGI(TAG, "编码吞吐: input=%.1ffps, output=%.1ffps, dropped=%lu, avg=%.1fms",
                         (produced - previous_produced) / seconds, encoded / seconds,
                         static_cast<unsigned long>(dropped - previous_dropped),
                         encoded ? encoding_us / (encoded * 1000.0) : 0.0);
                stats_started_us = stats_now_us;
                previous_produced = produced;
                previous_dropped = dropped;
                encoded = 0;
                encoding_us = 0;
            }

            if (task->type == kAudioTaskTypeEncodeToOpusQueue) {
                bool notify_consumer = false;
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    /* 开始上报时会推进 generation 并清空旧窗口。若一帧刚好
                     * 跨过这个边界，则丢弃它，避免把会话开始前的声音上报。 */
                    if (!audio_upload_enabled_ ||
                        task->upload_generation != upload_generation_) {
                        packet.reset();
                    } else {
                        if (opus_packet_queue_.size() >= MAX_OPUS_PACKETS_IN_QUEUE) {
                            opus_packet_queue_.pop_front();
                        }
                        opus_packet_queue_.push_back(std::move(packet));
                        notify_consumer = audio_upload_enabled_;
                    }
                }
                if (notify_consumer && upload_callbacks_.on_opus_packet_available) {
                    upload_callbacks_.on_opus_packet_available();
                }
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

// 下行包参数变化时重建解码器，并准备到 DAC 采样率的重采样。
void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

// ============================================================================
// 6. 上行编码队列与下行播放队列入口
// ============================================================================
// AFE 的实时帧满队列时丢旧帧，不阻塞采集。
void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (service_stopped_ ||
        (type == kAudioTaskTypeEncodeToOpusQueue && !audio_upload_enabled_ &&
         !local_encoding_benchmark_.load())) {
        return;
    }
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);

    /* Push the task to the encode queue */
    task->upload_generation = upload_generation_;

    if (type == kAudioTaskTypeEncodeToOpusQueue &&
        audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
        /* 麦克风数据是实时流，绝不能在 AFE 回调里等待。优先丢掉一个旧的
         * Opus 编码帧；如果队列全是测试任务，则直接丢掉当前帧。 */
        const auto oldest_opus = std::find_if(
            audio_encode_queue_.begin(), audio_encode_queue_.end(),
            [](const auto& queued) {
                return queued->type == kAudioTaskTypeEncodeToOpusQueue;
            });
        const bool dropped_queued_frame = oldest_opus != audio_encode_queue_.end();
        if (dropped_queued_frame) {
            audio_encode_queue_.erase(oldest_opus);
            ++encode_dropped_frames_;
        }
        if (!dropped_queued_frame) {
            return;
        }
    }
    if (type == kAudioTaskTypeEncodeToOpusQueue) {
        ++encode_produced_frames_;
        audio_encode_queue_.push_back(std::move(task));
        audio_queue_cv_.notify_all();
        return;
    }
    audio_queue_cv_.wait(lock, [this]() {
        return service_stopped_ ||
               audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE;
    });
    if (service_stopped_) return;
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

// 本地播放器入口：PCM 转为 codec 采样率后放入统一播放队列，wait 控制背压。
bool AudioService::PushPcm(const uint8_t* data, size_t size, int sample_rate,
                           const AudioOutputEvent& output, bool wait) {
    if (data == nullptr || size == 0 || (size % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "无效的播放器 PCM 数据: data=%p, bytes=%u", data,
                 static_cast<unsigned>(size));
        return false;
    }

    auto task = std::make_unique<AudioTask>();
    task->type = kAudioTaskTypePlayerToPlaybackQueue;
    task->player_output = output;
    const size_t input_samples = size / sizeof(int16_t);

    /* 两个播放器任务在通道切换时可能交接，重采样状态必须串行访问。 */
    {
        std::lock_guard<std::mutex> lock(player_resampler_mutex_);
        if (codec_ != nullptr && sample_rate > 0 && sample_rate != codec_->output_sample_rate()) {
            if (player_resampler_input_rate_ != sample_rate ||
                player_resampler_.output_sample_rate() != codec_->output_sample_rate()) {
                player_resampler_.Configure(sample_rate, codec_->output_sample_rate());
                player_resampler_input_rate_ = sample_rate;
            }
            const int target_size = player_resampler_.GetOutputSamples(input_samples);
            if (target_size <= 0 || !task->AllocatePlayerPcm(target_size)) {
                ESP_LOGE(TAG, "播放器 PCM PSRAM 分配失败: samples=%d", target_size);
                return false;
            }
            player_resampler_.Process(reinterpret_cast<const int16_t*>(data), input_samples,
                                      task->player_pcm);
        } else {
            if (!task->AllocatePlayerPcm(input_samples)) {
                ESP_LOGE(TAG, "播放器 PCM PSRAM 分配失败: bytes=%u",
                         static_cast<unsigned>(size));
                return false;
            }
            std::memcpy(task->player_pcm, data, size);
        }
    }
    const size_t pcm_bytes = task->PlaybackSamples() * sizeof(int16_t);

    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    const uint32_t enqueue_wait_start_ms = esp_log_timestamp();
    if (output.session_id == 0 || output.session_id != player_session_id_) return false;
    const bool task_queue_full =
        audio_playback_queue_.size() >= MAX_PLAYER_PLAYBACK_TASKS_IN_QUEUE;
    const bool byte_queue_full = player_max_buffered_pcm_bytes_ != 0 &&
                                 queued_player_pcm_bytes_ >=
                                     player_max_buffered_pcm_bytes_;
    const bool queue_was_full = task_queue_full || byte_queue_full;
    if (wait) {
        audio_queue_cv_.wait(lock, [this, queue_was_full, session = output.session_id]() {
            if (service_stopped_ || session != player_session_id_) {
                return true;
            }
            if (audio_playback_queue_.size() >= MAX_PLAYER_PLAYBACK_TASKS_IN_QUEUE) {
                return false;
            }
            if (player_max_buffered_pcm_bytes_ == 0) {
                return true;
            }
            const size_t threshold = queue_was_full
                                         ? player_producer_resume_pcm_bytes_
                                         : player_max_buffered_pcm_bytes_;
            return queued_player_pcm_bytes_ <= threshold;
        });
        const uint32_t enqueue_wait_ms = esp_log_timestamp() - enqueue_wait_start_ms;
        if (queue_was_full && enqueue_wait_ms >= 80) {
            ESP_LOGI(TAG,
                     "播放器生产受队列反压: wait=%lums, queue=%u, pcm=%u/%u",
                     static_cast<unsigned long>(enqueue_wait_ms),
                     static_cast<unsigned>(audio_playback_queue_.size()),
                     static_cast<unsigned>(queued_player_pcm_bytes_),
                     static_cast<unsigned>(player_max_buffered_pcm_bytes_));
        }
    } else if (task_queue_full || byte_queue_full) {
        return false;
    }

    if (service_stopped_ || output.session_id != player_session_id_) {
        return false;
    }
    audio_playback_queue_.push_back(std::move(task));
    queued_player_pcm_bytes_ += pcm_bytes;
    player_pcm_bytes_ += pcm_bytes;
    const size_t queue_size = audio_playback_queue_.size();
    const bool first_chunk = player_output_chunks_.load() == 0 && queue_size == 1;
    audio_queue_cv_.notify_all();
    if (first_chunk) {
        ESP_LOGI(TAG, "首个播放器 PCM 入队: bytes=%u, queue=%u",
                 static_cast<unsigned>(pcm_bytes), static_cast<unsigned>(queue_size));
    }
    return true;
}

size_t AudioService::BufferedPlaybackBytes(uint32_t expected_session) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return expected_session != 0 && player_session_id_ == expected_session &&
           player_stream_active_.load() ? queued_player_pcm_bytes_ : 0;
}

// 新播放器会话重置下行解码/缓冲状态；暂停保存的音乐 PCM 不能被新 TTS 会话清掉。
bool AudioService::BeginPlayback(const AudioPlaybackConfig& config) {
    if (config.session_id == 0 || !ResetPlayerPlaybackState(false, config.session_id)) return false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (player_session_id_ != config.session_id) return false;
        player_initial_prebuffer_bytes_ = config.initial_prebuffer_bytes;
        player_prebuffer_max_wait_ms_ = config.prebuffer_max_wait_ms;
        player_underrun_grace_ms_ = config.underrun_grace_ms;
        player_recovery_prebuffer_bytes_ = config.recovery_prebuffer_bytes != 0
                                               ? config.recovery_prebuffer_bytes
                                               : kAudioPlayerInitialPrebufferBytes;
        player_max_buffered_pcm_bytes_ = config.max_buffered_pcm_bytes;
        player_producer_resume_pcm_bytes_ =
            config.producer_resume_pcm_bytes != 0
                ? std::min(config.producer_resume_pcm_bytes,
                           config.max_buffered_pcm_bytes)
                : config.max_buffered_pcm_bytes;
        ESP_LOGI(TAG,
                 "播放器 PCM 水位: session=%lu, start=%u, recovery=%u, resume=%u, max=%u, wait=%lums",
                 static_cast<unsigned long>(config.session_id),
                 static_cast<unsigned>(player_initial_prebuffer_bytes_),
                 static_cast<unsigned>(player_recovery_prebuffer_bytes_),
                 static_cast<unsigned>(player_producer_resume_pcm_bytes_),
                 static_cast<unsigned>(player_max_buffered_pcm_bytes_),
                 static_cast<unsigned long>(player_prebuffer_max_wait_ms_));
        player_stream_active_.store(true);
    }
    return true;
}

void AudioService::SuspendPlayback() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    suspended_player_playback_queue_.clear();
    suspended_player_pcm_bytes_ = 0;
    suspended_player_recovery_prebuffer_bytes_ = player_recovery_prebuffer_bytes_;
    suspended_player_prebuffer_max_wait_ms_ = player_prebuffer_max_wait_ms_;
    suspended_player_underrun_grace_ms_ = player_underrun_grace_ms_;
    suspended_player_max_buffered_pcm_bytes_ = player_max_buffered_pcm_bytes_;
    suspended_player_producer_resume_pcm_bytes_ =
        player_producer_resume_pcm_bytes_;
    while (!audio_playback_queue_.empty()) {
        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        if (task != nullptr && task->type == kAudioTaskTypePlayerToPlaybackQueue) {
            suspended_player_pcm_bytes_ += task->PlaybackSamples() * sizeof(int16_t);
            suspended_player_playback_queue_.push_back(std::move(task));
        }
    }
    player_pcm_bytes_ = 0;
    queued_player_pcm_bytes_ = 0;
    player_playback_finished_ = false;
    player_stream_active_.store(false);
    audio_queue_cv_.notify_all();
    ESP_LOGI(TAG, "保存暂停音乐 PCM: tasks=%u, bytes=%u",
             static_cast<unsigned>(suspended_player_playback_queue_.size()),
             static_cast<unsigned>(suspended_player_pcm_bytes_));
}

void AudioService::ResumePlayback(const AudioOutputEvent& output) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    player_session_id_ = output.session_id;
    player_recovery_prebuffer_bytes_ = suspended_player_recovery_prebuffer_bytes_;
    player_prebuffer_max_wait_ms_ = suspended_player_prebuffer_max_wait_ms_;
    player_underrun_grace_ms_ = suspended_player_underrun_grace_ms_;
    player_max_buffered_pcm_bytes_ = suspended_player_max_buffered_pcm_bytes_;
    player_producer_resume_pcm_bytes_ =
        suspended_player_producer_resume_pcm_bytes_;
    if (suspended_player_playback_queue_.empty()) {
        player_playback_finished_ = false;
        player_initial_prebuffer_bytes_ = 0;
        player_stream_active_.store(true);
        return;
    }

    // TTS 已经排空活动队列。把暂停时尚未输出的音乐 PCM 放回队首，
    // 随后再恢复 GMF，这样恢复的第一个声音就是暂停点附近的音乐。
    audio_playback_queue_.clear();
    while (!suspended_player_playback_queue_.empty()) {
        audio_playback_queue_.push_back(
            std::move(suspended_player_playback_queue_.front()));
        suspended_player_playback_queue_.pop_front();
    }
    suspended_player_pcm_bytes_ = 0;
    player_pcm_bytes_ = 0;
    queued_player_pcm_bytes_ = 0;
    for (const auto& task : audio_playback_queue_) {
        if (task != nullptr) {
            const size_t task_bytes = task->PlaybackSamples() * sizeof(int16_t);
            player_pcm_bytes_ += task_bytes;
            queued_player_pcm_bytes_ += task_bytes;
        }
    }
    player_playback_finished_ = false;
    player_initial_prebuffer_bytes_ = 0;
    player_prebuffering_ = false;
    player_prebuffer_wait_logged_ = false;
    player_output_chunks_.store(0);
    player_output_last_activity_ms_.store(esp_log_timestamp());
    player_audio_clock_ms_.store(0);
    player_stream_active_.store(true);
    audio_queue_cv_.notify_all();
    ESP_LOGI(TAG, "恢复暂停音乐 PCM: session=%lu, tasks=%u, bytes=%u",
             static_cast<unsigned long>(output.session_id),
             static_cast<unsigned>(audio_playback_queue_.size()),
             static_cast<unsigned>(player_pcm_bytes_));
}

// 标记 PCM 已全部入队，使短尾帧越过预缓冲门槛，并补齐最后一个 DMA 缓冲。
void AudioService::MarkPlaybackFinished(const AudioOutputEvent& output) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (output.session_id != player_session_id_) return;
    player_playback_finished_ = true;

    /* i2s_channel_write() only submits full DMA buffers. Append silence to
     * commit the final partial buffer; the silence is inaudible and prevents
     * the end of a TTS response from being dropped by the I2S driver. */
    const size_t dma_buffer_bytes = codec_ != nullptr ? codec_->OutputDmaBufferBytes() : 0;
    const size_t remainder = dma_buffer_bytes > 0 ? player_pcm_bytes_ % dma_buffer_bytes : 0;
    if (remainder != 0) {
        const size_t padding_bytes = dma_buffer_bytes - remainder;
        auto padding = std::make_unique<AudioTask>();
        padding->type = kAudioTaskTypePlayerToPlaybackQueue;
        padding->player_output = output;
        if (!padding->AllocatePlayerPcm(padding_bytes / sizeof(int16_t))) {
            ESP_LOGE(TAG, "播放器末尾对齐 PCM PSRAM 分配失败: bytes=%u",
                     static_cast<unsigned>(padding_bytes));
            audio_queue_cv_.notify_all();
            return;
        }
        std::memset(padding->player_pcm, 0, padding_bytes);
        audio_playback_queue_.push_back(std::move(padding));
        queued_player_pcm_bytes_ += padding_bytes;
        player_pcm_bytes_ += padding_bytes;
        ESP_LOGI(TAG, "播放器末尾 DMA 对齐: padding=%u bytes",
                 static_cast<unsigned>(padding_bytes));
    }
    audio_queue_cv_.notify_all();
}

// 立即撤销活动队列；暂停保存的音乐 PCM 保留给后续恢复。
void AudioService::ClearActivePlaybackQueue(uint32_t expected_session) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (!OwnsPlaybackSession(player_session_id_, expected_session)) return;
    player_session_id_ = 0;
    player_stream_active_.store(false);
    audio_playback_queue_.clear();
    queued_player_pcm_bytes_ = 0;
    player_pcm_bytes_ = 0;
    audio_queue_cv_.notify_all();
}

// 立即撤销所有待播放 PCM，包括暂停保存的音乐。
void AudioService::ClearPlaybackQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    player_session_id_ = 0;
    player_stream_active_.store(false);
    audio_playback_queue_.clear();
    suspended_player_playback_queue_.clear();
    suspended_player_pcm_bytes_ = 0;
    queued_player_pcm_bytes_ = 0;
    suspended_player_recovery_prebuffer_bytes_ =
        kAudioPlayerInitialPrebufferBytes;
    suspended_player_max_buffered_pcm_bytes_ = 0;
    suspended_player_producer_resume_pcm_bytes_ = 0;
    player_pcm_bytes_ = 0;
    audio_queue_cv_.notify_all();
}

// 等到播放队列和当前 codec 写入结束，再等待 I2S DMA 环里的尾音实际送出。
void AudioService::WaitForPlaybackComplete(uint32_t expected_session) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this, expected_session]() {
        return service_stopped_ || !OwnsPlaybackSession(player_session_id_, expected_session) ||
               (audio_playback_queue_.empty() && !audio_output_busy_);
    });
    if (!OwnsPlaybackSession(player_session_id_, expected_session)) return;
    if (service_stopped_) {
        player_stream_active_.store(false);
        return;
    }

    /* Queue completion means the final DMA buffer has been accepted, not
     * necessarily transmitted. Wait for the configured DMA ring duration so
     * the application cannot resume listening over the TTS tail. */
    const int sample_rate = codec_ != nullptr ? codec_->output_sample_rate() : 0;
    if (sample_rate > 0) {
        const int drain_ms =
            (AUDIO_CODEC_DMA_DESC_NUM * AUDIO_CODEC_DMA_FRAME_NUM * 1000 + sample_rate - 1) /
            sample_rate;
        lock.unlock();
        vTaskDelay(pdMS_TO_TICKS(drain_ms));
        lock.lock();
        if (!OwnsPlaybackSession(player_session_id_, expected_session)) return;
        ESP_LOGI(TAG, "播放器 DMA 排空完成: drain=%dms", drain_ms);
    }
    player_stream_active_.store(false);
}

// 服务端 Opus 的下行入口；编码任务会解码并转送统一播放队列。
bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            return false;
        }
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

// ============================================================================
// 7. 上行与唤醒结果接口
// ============================================================================
// 网络消费入口：关闭上报时不取包，也不生成无消费者的上行 Opus。
std::unique_ptr<AudioStreamPacket> AudioService::PopOpusPacket() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (!audio_upload_enabled_ || opus_packet_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(opus_packet_queue_.front());
    opus_packet_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

const std::string& AudioService::GetLastWakeWord() const {
    static const std::string kEmptyWakeWord;
    if (!wake_word_) {
        return kEmptyWakeWord;
    }
    return wake_word_->GetLastDetectedWakeWord();
}

std::string AudioService::GetWakeWordName() const {
    if (!wake_word_name_.empty()) {
        return wake_word_name_;
    }
#if CONFIG_USE_MICRO_WAKE_WORD
    return kMicroWakeWordModelWakeWord;
#else
    return GetLastWakeWord();
#endif
}

// 只控制唤醒模型的运行与喂帧，不停止 ADC、AFE 或编码任务。
void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }
#if CONFIG_USE_MICRO_WAKE_WORD
    std::lock_guard<std::mutex> inference_lock(wake_inference_mutex_);
#endif
    if (enable && IsWakeWordRunning()) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
#if CONFIG_USE_MICRO_WAKE_WORD
        ClearProcessedWakeQueue();
#endif
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        wake_word_->Start();
        wake_word_running_.store(true);
    } else {
        /* 先关闭输入开关再 Stop，避免采集任务继续喂给唤醒算法。 */
        wake_word_running_.store(false);
#if CONFIG_USE_MICRO_WAKE_WORD
        ClearProcessedWakeQueue();
#endif
        wake_word_->Stop();
    }
}

// MicroWakeWord 使用轻量 Pause；其他实现退回普通 Stop。
void AudioService::PauseWakeWordDetection() {
#if CONFIG_USE_MICRO_WAKE_WORD
    if (!wake_word_) {
        return;
    }
    std::lock_guard<std::mutex> inference_lock(wake_inference_mutex_);
    wake_word_running_.store(false);
    ClearProcessedWakeQueue();
    static_cast<MicroWakeWord*>(wake_word_.get())->Pause();
#else
    EnableWakeWordDetection(false);
#endif
}

// 上报会话边界：开启时清掉此前缓存，并用 generation 排除正在编码的旧帧。
// 开关只影响上行编码/消费；ADC、AFE/NS、唤醒和下行解码始终常驻。
void AudioService::EnableAudioUpload(bool enable) {
    bool previous = false;
    size_t encode_before = 0;
    size_t opus_before = 0;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        // started / 重连等入口也不能绕过输入保护，tone 期间不编码静音占位包。
        if (input_discard_state_.load() & 1u) enable = false;
        if (enable) local_encoding_benchmark_.store(false);
        previous = audio_upload_enabled_;
        if (previous == enable) {
            return;
        }
        encode_before = audio_encode_queue_.size();
        opus_before = opus_packet_queue_.size();
        audio_upload_enabled_ = enable;
        generation = ++upload_generation_;
        // 两个方向的会话边界都清缓存；新会话不能接收旧会话的在途编码帧。
        audio_encode_queue_.clear();
        opus_packet_queue_.clear();
        audio_queue_cv_.notify_all();
    }
    ESP_LOGI(TAG,
             "音频上报: %d->%d, generation=%lu, 清理=pcm:%u/opus:%u, "
             "处理链常驻=%d",
             previous ? 1 : 0, enable ? 1 : 0,
             static_cast<unsigned long>(generation),
             static_cast<unsigned>(encode_before),
             static_cast<unsigned>(opus_before),
             service_stopped_.load() ? 0 : 1);
}

void AudioService::SetMicrophoneSuppressed(bool suppress) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    const auto previous = input_discard_state_.load();
    const auto next = NextInputDiscardState(previous, suppress);
    if (next == previous) return;
    input_discard_state_.store(next);
    // 两个边界都作废在途编码；解除保护仅允许新数据，业务层另行开上报。
    audio_upload_enabled_ = false;
    ++upload_generation_;
    audio_encode_queue_.clear();
    opus_packet_queue_.clear();
    audio_queue_cv_.notify_all();
    ESP_LOGI(TAG, "麦克风输入保护: %s，丢弃原始输入并清空上报缓存",
             suppress ? "开启" : "解除");
}

bool AudioService::SetLocalEncodingBenchmark(bool enable) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_upload_enabled_ || service_stopped_) return false;
    if (local_encoding_benchmark_.exchange(enable) == enable) return true;
    ++upload_generation_;
    audio_encode_queue_.clear();
    opus_packet_queue_.clear();
    audio_queue_cv_.notify_all();
    ESP_LOGI(TAG, "本地编码压力测试: %s，16kHz/20ms/16kbps Opus，不发送网络音频",
             enable ? "开启" : "关闭");
    return true;
}

// ============================================================================
// 8. 运行时开关、回调与本地播放
// ============================================================================
// AEC 同时要求编译支持、codec 全双工以及 ADC 参考声道。
bool AudioService::SupportsAec() const {
#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_AUDIO_PROCESSOR
    return codec_ && codec_->duplex() && codec_->input_reference();
#else
    return false;
#endif
}

// 录音测试从云端编码器的 AFE/AEC PCM 输入点取帧。停止后依次播放
// 原始 PCM、双提示音和同参数 Opus 编解码结果，便于定位音质损伤。
void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (audio_testing_running_.load() || audio_testing_processing_.load()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(audio_testing_pcm_mutex_);
            audio_testing_pcm_buffer_.clear();
            audio_testing_mic_stats_ = {};
            audio_testing_ref_stats_ = {};
            constexpr size_t kMaxTestingSamples =
                AUDIO_TESTING_MAX_DURATION_MS * 16000 / 1000;
            audio_testing_pcm_buffer_.reserve(kMaxTestingSamples);
            audio_testing_elapsed_us_ = 0;
            audio_testing_input_overflows_ = -1;
        }
        if (audio_testing_encoder_ != nullptr) {
            audio_testing_encoder_->ResetState();
        }
        ESP_LOGI(TAG, "音频测试采样点: AFE/AEC 输出，16 kHz 单声道");
        {
            std::lock_guard<std::mutex> lock(audio_testing_pcm_mutex_);
            // 复位编码器和日志不属于录音时间，与实际取样开关保持同一边界。
            audio_testing_input_overflows_start_ = codec_->input_overflow_count();
            audio_testing_start_us_ = esp_timer_get_time();
            audio_testing_running_.store(true);
        }
    } else {
        if (!audio_testing_running_.exchange(false)) {
            return;
        }
        // 先冻结边界，再等待统计锁；CPU 调度/取锁的延迟不能算进录音时长。
        const int64_t stopped_us = esp_timer_get_time();
        const int64_t overflows = codec_->input_overflow_count();
        {
            std::lock_guard<std::mutex> lock(audio_testing_pcm_mutex_);
            audio_testing_elapsed_us_ = stopped_us - audio_testing_start_us_;
            if (overflows >= 0 && audio_testing_input_overflows_start_ >= 0) {
                audio_testing_input_overflows_ = static_cast<uint32_t>(overflows) -
                    static_cast<uint32_t>(audio_testing_input_overflows_start_);
            }
        }
        if (audio_testing_processing_.exchange(true)) {
            ESP_LOGW(TAG, "音频测试 A/B 回放仍在准备中");
            return;
        }
        BaseType_t result = xTaskCreateWithCaps([](void* arg) {
            auto* service = static_cast<AudioService*>(arg);
            service->PrepareAudioTestingPlayback();
            ESP_LOGI(TAG, "音频测试任务最小剩余栈: %u bytes",
                     static_cast<unsigned>(
                         uxTaskGetStackHighWaterMark(nullptr)));
            service->audio_testing_task_handle_ = nullptr;
            service->audio_testing_processing_.store(false);
            vTaskDeleteWithCaps(nullptr);
        }, "audio_test_ab", 2048 * 16, this, kAudioCodecPriority,
           &audio_testing_task_handle_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (result != pdPASS) {
            audio_testing_task_handle_ = nullptr;
            audio_testing_processing_.store(false);
            std::lock_guard<std::mutex> lock(audio_testing_pcm_mutex_);
            audio_testing_pcm_buffer_.clear();
            ESP_LOGE(TAG, "创建音频测试 A/B 编解码任务失败");
        }
    }
}

// Opus 编解码的栈需求明显高于 Application 主任务，因此 A/B 回放准备
// 必须在独立的大栈任务中完成，不能在按键事件回调栈上同步执行。
void AudioService::PrepareAudioTestingPlayback() {
        std::deque<std::unique_ptr<AudioTask>> playback_tasks;
        uint64_t square_sum = 0;
        int32_t peak = 0;
        size_t clipped_samples = 0;
        size_t recorded_samples = 0;
        size_t comparable_samples = 0;
        size_t opus_packet_count = 0;
        size_t opus_payload_bytes = 0;
        {
            std::lock_guard<std::mutex> lock(audio_testing_pcm_mutex_);
            auto log_input = [](const char* label, const TestingInputStats& stats) {
                if (stats.samples == 0) return;
                const double mean = static_cast<double>(stats.sum) / stats.samples;
                const double power = static_cast<double>(stats.square_sum) / stats.samples;
                const double rms = std::sqrt(power);
                const double ac_rms = std::sqrt(std::max(0.0, power - mean * mean));
                ESP_LOGI(TAG,
                         "音频测试 ADC %s: samples=%u, peak=%ld, dc=%.1f, "
                         "rms=%.1fdBFS, ac_rms=%.1fdBFS",
                         label, static_cast<unsigned>(stats.samples),
                         static_cast<long>(stats.peak), mean,
                         rms > 0 ? 20.0 * std::log10(rms / 32768.0) : -120.0,
                         ac_rms > 0 ? 20.0 * std::log10(ac_rms / 32768.0) : -120.0);
            };
            log_input("MIC", audio_testing_mic_stats_);
            log_input("REF", audio_testing_ref_stats_);
            recorded_samples = audio_testing_pcm_buffer_.size();
            const double wall_ms = audio_testing_elapsed_us_ / 1000.0;
            const double adc_ms = audio_testing_mic_stats_.samples / 16.0;
            const double expected_ms = std::min(
                wall_ms, static_cast<double>(AUDIO_TESTING_MAX_DURATION_MS));
            const double coverage = expected_ms > 0 ? adc_ms / expected_ms * 100.0 : 0;
            ESP_LOGI(TAG,
                     "音频测试时长: wall=%.1fms, ADC=%.1fms, AFE=%.1fms, "
                     "coverage=%.1f%%, RX_overflows=%ld (-1=unsupported)",
                     wall_ms, adc_ms, recorded_samples / 16.0, coverage,
                     static_cast<long>(audio_testing_input_overflows_));
            if (expected_ms >= 1000 && coverage < 95.0) {
                ESP_LOGW(TAG, "音频采集不足: 检查采集调度/DMA，回放可能出现跳字或加速感");
            }
            for (const int16_t sample : audio_testing_pcm_buffer_) {
                const int32_t value = sample;
                const int32_t magnitude = value < 0 ? -value : value;
                peak = std::max(peak, magnitude);
                square_sum += static_cast<uint64_t>(value * value);
                if (sample == INT16_MIN || sample == INT16_MAX) {
                    ++clipped_samples;
                }
            }

            constexpr size_t kOpusFrameSamples =
                OPUS_FRAME_DURATION_MS * 16000 / 1000;
            comparable_samples = recorded_samples -
                                 recorded_samples % kOpusFrameSamples;

            // 低分位帧电平更适合评估停顿段，避免整段 RMS 被人声或瞬态主导。
            std::vector<double> frame_levels;
            for (size_t offset = 0; offset < comparable_samples;
                 offset += kOpusFrameSamples) {
                uint64_t frame_power = 0;
                for (size_t i = 0; i < kOpusFrameSamples; ++i) {
                    const int32_t value = audio_testing_pcm_buffer_[offset + i];
                    frame_power += static_cast<uint64_t>(value * value);
                }
                const double frame_rms = std::sqrt(
                    static_cast<double>(frame_power) / kOpusFrameSamples);
                frame_levels.push_back(frame_rms > 0
                    ? 20.0 * std::log10(frame_rms / 32768.0) : -120.0);
            }
            if (!frame_levels.empty()) {
                std::sort(frame_levels.begin(), frame_levels.end());
                const size_t last = frame_levels.size() - 1;
                ESP_LOGI(TAG,
                         "音频测试 AFE 帧电平: p10=%.1f, p50=%.1f, p90=%.1f dBFS",
                         frame_levels[last / 10], frame_levels[last / 2],
                         frame_levels[last * 9 / 10]);
            }

            // 100 ms 一块，在保持按键响应的同时减少 PSRAM 小块分配。
            constexpr size_t kPlaybackChunkSamples = 16000 / 10;
            auto append_playback_tasks = [&](const int16_t* pcm, size_t total,
                                             const char* label) {
                for (size_t offset = 0; offset < total;
                     offset += kPlaybackChunkSamples) {
                    const size_t samples = std::min(
                        kPlaybackChunkSamples, total - offset);
                    auto task = std::make_unique<AudioTask>();
                    task->type = kAudioTaskTypeDecodeToPlaybackQueue;
                    task->testing_playback = true;
                    if (!task->AllocatePlayerPcm(samples)) {
                        ESP_LOGE(TAG,
                                 "音频测试%s回放分配失败: offset=%u, samples=%u",
                                 label, static_cast<unsigned>(offset),
                                 static_cast<unsigned>(samples));
                        return false;
                    }
                    std::memcpy(task->player_pcm, pcm + offset,
                                samples * sizeof(int16_t));
                    playback_tasks.push_back(std::move(task));
                }
                return true;
            };

            // 第一段：直接回放云端编码器实际收到的 AFE/AEC PCM。
            const bool pcm_ready = comparable_samples > 0 &&
                append_playback_tasks(audio_testing_pcm_buffer_.data(),
                                      comparable_samples, " PCM");

            // 使用独立状态但完全相同参数的编码器进行本地 Opus 往返。
            std::vector<int16_t> opus_roundtrip_pcm;
            opus_roundtrip_pcm.reserve(comparable_samples);
            if (pcm_ready && audio_testing_encoder_ != nullptr) {
                audio_testing_encoder_->ResetState();
                OpusDecoderWrapper testing_decoder(
                    16000, 1, OPUS_FRAME_DURATION_MS);
                testing_decoder.ResetState();
                for (size_t offset = 0; offset < comparable_samples;
                     offset += kOpusFrameSamples) {
                    if (service_stopped_.load()) break;
                    // 离线编码整段录音时周期性让出 CPU，保证常驻采集继续运行。
                    if (offset % (kOpusFrameSamples * 5) == 0) vTaskDelay(1);
                    std::vector<int16_t> frame(
                        audio_testing_pcm_buffer_.begin() + offset,
                        audio_testing_pcm_buffer_.begin() + offset +
                            kOpusFrameSamples);
                    std::vector<uint8_t> opus_payload;
                    if (!audio_testing_encoder_->Encode(
                            std::move(frame), opus_payload)) {
                        ESP_LOGE(TAG,
                                 "音频测试 Opus 编码失败: frame=%u",
                                 static_cast<unsigned>(opus_packet_count));
                        break;
                    }
                    opus_payload_bytes += opus_payload.size();
                    ++opus_packet_count;
                    std::vector<int16_t> decoded;
                    if (!testing_decoder.Decode(std::move(opus_payload),
                                                decoded)) {
                        ESP_LOGE(TAG,
                                 "音频测试 Opus 解码失败: frame=%u",
                                 static_cast<unsigned>(opus_packet_count));
                        break;
                    }
                    opus_roundtrip_pcm.insert(opus_roundtrip_pcm.end(),
                                              decoded.begin(), decoded.end());
                }
            }

            if (!opus_roundtrip_pcm.empty()) {
                // 双短音明确标出第二段开始，前后留白避免与语音粘连。
                constexpr size_t kMarkerSamples = 16000 * 3 / 5;
                constexpr size_t kBeepSamples = 16000 / 10;
                constexpr size_t kGapSamples = 16000 / 20;
                constexpr size_t kLeadSamples = 16000 * 3 / 20;
                constexpr double kTwoPi = 6.28318530717958647692;
                std::vector<int16_t> marker(kMarkerSamples, 0);
                auto fill_beep = [&](size_t begin) {
                    for (size_t i = 0; i < kBeepSamples; ++i) {
                        const size_t fade = std::min(
                            {i, kBeepSamples - 1 - i,
                             static_cast<size_t>(80)});
                        const double envelope =
                            std::min(1.0, static_cast<double>(fade) / 80.0);
                        marker[begin + i] = static_cast<int16_t>(
                            6000.0 * envelope *
                            std::sin(kTwoPi * 880.0 * i / 16000.0));
                    }
                };
                fill_beep(kLeadSamples);
                fill_beep(kLeadSamples + kBeepSamples + kGapSamples);
                append_playback_tasks(marker.data(), marker.size(), "提示音");
                append_playback_tasks(opus_roundtrip_pcm.data(),
                                      opus_roundtrip_pcm.size(), " Opus");
            }
            audio_testing_pcm_buffer_.clear();
        }

        const double duration_ms = recorded_samples * 1000.0 / 16000.0;
        const double rms = recorded_samples == 0
            ? 0.0
            : std::sqrt(static_cast<double>(square_sum) / recorded_samples);
        const double peak_dbfs = peak == 0
            ? -120.0
            : 20.0 * std::log10(static_cast<double>(peak) / 32768.0);
        const double rms_dbfs = rms == 0.0
            ? -120.0
            : 20.0 * std::log10(rms / 32768.0);
        const double clipped_percent = recorded_samples == 0
            ? 0.0
            : clipped_samples * 100.0 / recorded_samples;
        ESP_LOGI(TAG,
                 "音频测试 A/B: duration=%.0fms, samples=%u/%u, peak=%ld "
                 "(%.1fdBFS), rms=%.1fdBFS, clipped=%u (%.2f%%), "
                 "opus_packets=%u, opus_bytes=%u, playback_chunks=%u",
                 duration_ms, static_cast<unsigned>(recorded_samples),
                 static_cast<unsigned>(comparable_samples),
                 static_cast<long>(peak), peak_dbfs, rms_dbfs,
                 static_cast<unsigned>(clipped_samples), clipped_percent,
                 static_cast<unsigned>(opus_packet_count),
                 static_cast<unsigned>(opus_payload_bytes),
                 static_cast<unsigned>(playback_tasks.size()));
        if (clipped_samples > 0) {
            ESP_LOGW(TAG,
                     "音频测试检测到硬削波；若回放仍有爆音，应降低 PCM 增益");
        }
        if (comparable_samples > 0 && opus_packet_count > 0) {
            ESP_LOGI(TAG,
                     "音频测试回放顺序: AFE/AEC PCM -> 双提示音 -> 16kbps Opus");
        }

        {
            std::lock_guard<std::mutex> lock(audio_queue_mutex_);
            if (service_stopped_.load()) {
                return;
            }
            while (!playback_tasks.empty()) {
                audio_playback_queue_.push_back(
                    std::move(playback_tasks.front()));
                playback_tasks.pop_front();
            }
            audio_queue_cv_.notify_all();
        }
}

// 根据硬件能力切换 AEC；不改变 MicroWakeWord 的后处理输入点，NS 继续运行。
void AudioService::SetAecEnabled(bool enable) {
    aec_enabled_ = enable && SupportsAec();
    if (enable && !aec_enabled_) {
        ESP_LOGW(TAG, "当前板型不支持 AEC，保持关闭");
    }
    if (audio_processor_initialized_) {
        audio_processor_->SetAecEnabled(aec_enabled_);
    }
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::SetUploadCallbacks(AudioServiceUploadCallbacks& callbacks) {
    upload_callbacks_ = callbacks;
}

void AudioService::SetOutputCallbacks(AudioServiceOutputCallbacks& callbacks) {
    output_callbacks_ = callbacks;
}

// 本地 Ogg/Opus 提示音入口：拆出 Opus 包后仍交给统一的下行解码/播放链。
void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const uint8_t* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();
    size_t offset = 0;

    auto find_page = [&](size_t start)->size_t {
        for (size_t i = start; i + 4 <= size; ++i) {
            if (buf[i] == 'O' && buf[i+1] == 'g' && buf[i+2] == 'g' && buf[i+3] == 'S') return i;
        }
        return static_cast<size_t>(-1);
    };

    bool seen_head = false;
    bool seen_tags = false;
    int sample_rate = 16000; // 默认值

    while (true) {
        size_t pos = find_page(offset);
        if (pos == static_cast<size_t>(-1)) break;
        offset = pos;
        if (offset + 27 > size) break;

        const uint8_t* page = buf + offset;
        uint8_t page_segments = page[26];
        size_t seg_table_off = offset + 27;
        if (seg_table_off + page_segments > size) break;

        size_t body_size = 0;
        for (size_t i = 0; i < page_segments; ++i) body_size += page[27 + i];

        size_t body_off = seg_table_off + page_segments;
        if (body_off + body_size > size) break;

        // Parse packets using lacing
        size_t cur = body_off;
        size_t seg_idx = 0;
        while (seg_idx < page_segments) {
            size_t pkt_len = 0;
            size_t pkt_start = cur;
            bool continued = false;
            do {
                uint8_t l = page[27 + seg_idx++];
                pkt_len += l;
                cur += l;
                continued = (l == 255);
            } while (continued && seg_idx < page_segments);

            if (pkt_len == 0) continue;
            const uint8_t* pkt_ptr = buf + pkt_start;

            if (!seen_head) {
                // 解析OpusHead包
                if (pkt_len >= 19 && std::memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                    seen_head = true;
                    
                    // OpusHead结构：[0-7] "OpusHead", [8] version, [9] channel_count, [10-11] pre_skip
                    // [12-15] input_sample_rate, [16-17] output_gain, [18] mapping_family
                    if (pkt_len >= 12) {
                        uint8_t version = pkt_ptr[8];
                        uint8_t channel_count = pkt_ptr[9];
                        
                        if (pkt_len >= 16) {
                            // 读取输入采样率 (little-endian)
                            sample_rate = pkt_ptr[12] | (pkt_ptr[13] << 8) | 
                                        (pkt_ptr[14] << 16) | (pkt_ptr[15] << 24);
                            ESP_LOGI(TAG, "OpusHead: version=%d, channels=%d, sample_rate=%d", 
                                   version, channel_count, sample_rate);
                        }
                    }
                }
                continue;
            }
            if (!seen_tags) {
                // Expect OpusTags in second packet
                if (pkt_len >= 8 && std::memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                    seen_tags = true;
                }
                continue;
            }

            // Audio packet (Opus)
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            packet->payload.resize(pkt_len);
            std::memcpy(packet->payload.data(), pkt_ptr, pkt_len);
            PushPacketToDecodeQueue(std::move(packet), true);
        }

        offset = body_off + body_size;
    }
}

// ============================================================================
// 9. 播放状态、功耗和唤醒资源
// ============================================================================
bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return !audio_testing_running_.load() &&
           !audio_testing_processing_.load() &&
           audio_encode_queue_.empty() && audio_decode_queue_.empty() &&
           audio_playback_queue_.empty() && suspended_player_playback_queue_.empty();
}

// 清理一次下行播放会话的解码器、重采样器、播放队列和首帧统计。
void AudioService::ResetDecoder() {
    ResetPlayerPlaybackState(true);
}

bool AudioService::ResetPlayerPlaybackState(bool clear_suspended_queue, uint32_t new_session) {
    std::scoped_lock lock(player_resampler_mutex_, audio_queue_mutex_);
    if (new_session != 0) {
        if (!IsNewerPlaybackSession(new_session, latest_player_session_id_)) return false;
        latest_player_session_id_ = new_session;
    }
    player_session_id_ = new_session;
    opus_decoder_->ResetState();
    /* 新播放会话以干净的重采样状态开始，避免上一段音频的残留相位 */
    player_resampler_input_rate_ = 0;
    player_output_chunks_.store(0);
    player_output_last_activity_ms_.store(0);
    player_audio_clock_ms_.store(0);
    player_pcm_bytes_ = 0;
    queued_player_pcm_bytes_ = 0;
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    if (clear_suspended_queue) {
        suspended_player_playback_queue_.clear();
        suspended_player_pcm_bytes_ = 0;
        suspended_player_recovery_prebuffer_bytes_ =
            kAudioPlayerInitialPrebufferBytes;
        suspended_player_max_buffered_pcm_bytes_ = 0;
        suspended_player_producer_resume_pcm_bytes_ = 0;
    }
    player_playback_finished_ = false;
    player_initial_prebuffer_bytes_ = 0;
    player_recovery_prebuffer_bytes_ = kAudioPlayerInitialPrebufferBytes;
    player_prebuffer_max_wait_ms_ = 0;
    player_underrun_grace_ms_ = 0;
    player_max_buffered_pcm_bytes_ = 0;
    player_producer_resume_pcm_bytes_ = 0;
    player_prebuffering_ = true;
    player_prebuffer_wait_logged_ = false;
    player_rebuffer_count_ = 0;
    player_stream_active_.store(false);
    audio_queue_cv_.notify_all();
    return true;
}

// 定时关闭长时间无读写的 codec 输入/输出；有新数据时对应路径会重新启用。
void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled() &&
        !player_stream_active_.load()) {
        codec_->EnableOutput(false);
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

// 从资源分区寻找唤醒模型；MicroWakeWord 用 JSON/TFLite，ESP-SR 用模型包。
bool AudioService::LoadWakeWordResources() {
#if CONFIG_USE_MICRO_WAKE_WORD
    std::vector<std::string> paths;
    if (!Resource::List(ResourcePackage::WakeWord, "/", paths)) {
        ESP_LOGW(TAG, "The wake_word resource partition is unavailable");
        return false;
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& descriptor_path : paths) {
        if (!HasSuffix(descriptor_path, ".json")) {
            continue;
        }

        ResourceData descriptor_asset;
        if (!Resource::Get(ResourcePackage::WakeWord, descriptor_path,
                                descriptor_asset)) {
            continue;
        }
        cJSON* descriptor = cJSON_ParseWithLength(
            reinterpret_cast<const char*>(descriptor_asset.address), descriptor_asset.size);
        if (descriptor == nullptr) {
            ESP_LOGW(TAG, "Invalid wake-word descriptor: %s", descriptor_path.c_str());
            continue;
        }

        const cJSON* wake_word = cJSON_GetObjectItemCaseSensitive(descriptor, "wake_word");
        if (!cJSON_IsString(wake_word) || wake_word->valuestring == nullptr) {
            cJSON_Delete(descriptor);
            continue;
        }
        MicroWakeWordConfig config;
        config.wake_word = wake_word->valuestring;
        if (!ParseMicroWakeWordConfig(descriptor, config)) {
            ESP_LOGW(TAG,
                     "Wake-word descriptor has no valid micro.probability_cutoff: %s",
                     descriptor_path.c_str());
            cJSON_Delete(descriptor);
            continue;
        }

        std::string model_path = ModelPathForDescriptor(descriptor_path);
        ResourceData model_asset;
        bool found = !model_path.empty() &&
                     Resource::Get(ResourcePackage::WakeWord, model_path, model_asset);
        if (!found) {
            const cJSON* model = cJSON_GetObjectItemCaseSensitive(descriptor, "model");
            if (cJSON_IsString(model) && model->valuestring != nullptr) {
                model_path = model->valuestring;
                if (model_path.empty() || model_path.front() != '/') {
                    const size_t slash = descriptor_path.find_last_of('/');
                    model_path = descriptor_path.substr(0, slash + 1) + model_path;
                }
                found = Resource::Get(ResourcePackage::WakeWord, model_path, model_asset);
            }
        }
        if (found) {
            SetMicroWakeWordModel(model_asset.address, model_asset.size,
                                  config.wake_word, config);
            ESP_LOGI(TAG, "Loaded MWW model from %s (%s)", model_path.c_str(),
                     config.wake_word.c_str());
            cJSON_Delete(descriptor);
            return true;
        }
        cJSON_Delete(descriptor);
    }
    ESP_LOGW(TAG, "The wake_word descriptor or TFLite model is missing");
    return false;
#else
    // 每次调用都先释放旧实现，便于 OTA 后直接重新读取资源。
    EnableWakeWordDetection(false);
    const bool model_owned_by_detector = wake_word_ != nullptr;
    srmodel_list_t* old_models = models_list_;
    wake_word_.reset();
    wake_word_initialized_ = false;
    models_list_ = nullptr;
    if (!model_owned_by_detector && old_models != nullptr) {
        esp_srmodel_deinit(old_models);
    }

    constexpr const char* kCandidates[] = {"/srmodels.bin", "/cae_esr.bin"};
    for (const char* candidate : kCandidates) {
        ResourceData asset;
        if (!Resource::Get(ResourcePackage::WakeWord, candidate, asset)) {
            continue;
        }
        auto* models = srmodel_load(const_cast<uint8_t*>(asset.address));
        if (models != nullptr) {
            SetModelsList(models);
            ESP_LOGI(TAG, "Loaded ESP-SR model from %s", candidate);
            return true;
        }
    }
    ESP_LOGW(TAG, "The wake_word ESP-SR model is missing");
    return false;
#endif
}

#if CONFIG_USE_MICRO_WAKE_WORD
// 替换模型时先关闭检测；真正 Initialize/Start 会在再次启用检测时执行。
void AudioService::SetMicroWakeWordModel(const void* model_data, size_t model_size,
                                         const std::string& wake_word,
                                         const MicroWakeWordConfig& config) {
    EnableWakeWordDetection(false);
    wake_word_.reset();
    wake_word_initialized_ = false;

    static srmodel_list_t models_list = {.num = 0};
    models_list_ = &models_list;

    wake_word_ = std::make_unique<MicroWakeWord>(model_data, model_size, config);
    wake_word_name_ = wake_word;
    wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
        if (local_encoding_benchmark_.load()) {
            ESP_LOGI(TAG, "本地压力测试捕获唤醒: %s (不创建云端会话)", wake_word.c_str());
            return;
        }
        // The callback runs under MicroWakeWord's model lock, so Stop() would
        // deadlock here. Stop feeding immediately; the application task will
        // reset and restart the detector synchronously.
        wake_word_running_.store(false);
        if (callbacks_.on_wake_word_detected) {
            callbacks_.on_wake_word_detected(wake_word);
        }
    });
}
#else // !CONFIG_USE_MICRO_WAKE_WORD
// ESP-SR 分支按模型包内容选择对应唤醒实现。
void AudioService::SetModelsList(srmodel_list_t* models_list) {
    EnableWakeWordDetection(false);
    wake_word_.reset();
    wake_word_initialized_ = false;
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#else
    if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            wake_word_running_.store(false);
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
}
#endif // CONFIG_USE_MICRO_WAKE_WORD

bool AudioService::IsAfeWakeWord() {
#if CONFIG_USE_MICRO_WAKE_WORD
    return false;
#else // CONFIG_USE_MICRO_WAKE_WORD
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    return wake_word_ != nullptr && dynamic_cast<AfeWakeWord*>(wake_word_.get()) != nullptr;
#else
    return false;
#endif
#endif // CONFIG_USE_MICRO_WAKE_WORD
}
