#include "audio_service.h"
#include "resource/resource.h"
#include "xiaoling/conn/xiaoling_config.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <algorithm>
#include <climits>
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
constexpr UBaseType_t kAudioCodecPriority = 11;
constexpr UBaseType_t kAudioAlgorithmPriority = 8;
constexpr uint32_t kAudioInputIdleYieldIntervalMs = 500;

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

void ParseMicroWakeWordConfig(const cJSON* descriptor, MicroWakeWordConfig& config) {
    const cJSON* micro = cJSON_GetObjectItemCaseSensitive(descriptor, "micro");
    if (!cJSON_IsObject(micro)) {
        return;
    }
    const cJSON* cutoff = cJSON_GetObjectItemCaseSensitive(micro, "probability_cutoff");
    if (cJSON_IsNumber(cutoff) && cutoff->valuedouble > 0.0 && cutoff->valuedouble <= 1.0) {
        config.probability_cutoff = static_cast<float>(cutoff->valuedouble);
    }
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
        if (wake_uses_processor_output_.load() && IsWakeWordRunning()) {
            std::lock_guard<std::mutex> lock(processed_wake_mutex_);
            if (processed_wake_queue_.size() >= MAX_PROCESSED_WAKE_FRAMES_IN_QUEUE) {
                processed_wake_queue_.pop_front();
                static uint32_t dropped_frames = 0;
                if ((++dropped_frames % 100) == 1) {
                    ESP_LOGW(TAG, "Processed wake queue full, dropped frames: %lu",
                             static_cast<unsigned long>(dropped_frames));
                }
            }
            processed_wake_queue_.push_back(data);
            processed_wake_cv_.notify_one();
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
     * 音频处理链只初始化一次。Start() 之后 ADC -> AFE/AEC -> Opus 始终运行，
     * 业务层只决定是否消费 Opus，不再负责反复创建或停止算法任务。
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

    audio_processor_->SetAecEnabled(aec_enabled_);
    audio_processor_->Start();
#if CONFIG_USE_MICRO_WAKE_WORD
    SetWakeInputFromProcessor(aec_enabled_ && SupportsAec());
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
    }, "processed_wake", 2048 * 3, this, kAudioAlgorithmPriority,
       &processed_wake_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (wake_task_result != pdPASS) {
        ESP_LOGW(TAG, "PSRAM 唤醒任务栈分配失败，回退内部 SRAM");
        xTaskCreatePinnedToCore([](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->ProcessedWakeTask();
            vTaskDelete(nullptr);
        }, "processed_wake", 2048 * 3, this, kAudioAlgorithmPriority,
           &processed_wake_task_handle_, 0);
    }
#endif

    ESP_LOGI(TAG,
             "音频任务优先级: output=%u, opus=%u, input=%u, wake=%u, afe=%u",
             static_cast<unsigned>(kAudioCodecPriority),
             static_cast<unsigned>(kAudioCodecPriority),
             static_cast<unsigned>(kAudioAlgorithmPriority),
             static_cast<unsigned>(kAudioAlgorithmPriority),
             static_cast<unsigned>(kAudioAlgorithmPriority));
}

// 系统级暂停（目前由 OTA 使用）：通知任务退出并清空队列；普通业务切换不调用。
void AudioService::Stop() {
    if (service_stopped_.exchange(true)) {
        return;
    }
    esp_timer_stop(audio_power_timer_);
    audio_testing_running_.store(false);
    wake_word_running_.store(false);
    if (audio_processor_initialized_) {
        audio_processor_->Stop();
    }
#if CONFIG_USE_MICRO_WAKE_WORD
    SetWakeInputFromProcessor(false);
#endif
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_upload_enabled_ = false;
    ++upload_generation_;
    audio_encode_queue_.clear();
    opus_packet_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
#if CONFIG_USE_MICRO_WAKE_WORD
    ClearProcessedWakeQueue();
    processed_wake_cv_.notify_all();
#endif
}

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

    // Amplify the microphone before it is split to the wake-word and AFE pipelines.
    constexpr int32_t kMicPcmGain = 5;
    const size_t input_channels = codec_->input_channels();
    for (size_t i = 0; i < data.size(); i += input_channels) {
        const int32_t amplified = static_cast<int32_t>(data[i]) * kMicPcmGain;
        data[i] = static_cast<int16_t>(std::clamp(amplified,
            static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    return true;
}

// 常驻采集入口：取各消费者所需的最小帧长读 ADC，再分别拼成测试、AFE、唤醒帧。
// 处理器总是接收输入；wake_running 只决定是否向唤醒算法喂帧。
void AudioService::AudioInputTask() {
    std::vector<int16_t> wake_pending;
    std::vector<int16_t> processor_pending;
    std::vector<int16_t> testing_pending;
    TickType_t last_idle_yield_tick = xTaskGetTickCount();
    while (!service_stopped_.load()) {
        /* I2S input and synchronous AFE/AEC feed can form a continuously-ready
         * loop while full-duplex upload is active. Periodically block for one
         * scheduler tick so CPU0's Idle task can service the task watchdog.
         * The codec DMA ring is substantially longer than this 10 ms tick. */
        const TickType_t now_tick = xTaskGetTickCount();
        if (now_tick - last_idle_yield_tick >=
            pdMS_TO_TICKS(kAudioInputIdleYieldIntervalMs)) {
            vTaskDelay(1);
            last_idle_yield_tick = xTaskGetTickCount();
        }

        const bool testing_running = audio_testing_running_.load();
        if (testing_running) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
        } else testing_pending.clear();

        const bool wake_running = wake_word_running_.load();
#if CONFIG_USE_MICRO_WAKE_WORD
        const bool use_processed_wake = wake_uses_processor_output_.load();
#else
        const bool use_processed_wake = false;
#endif
        if (!wake_running) wake_pending.clear();
        if (use_processed_wake) wake_pending.clear();

        // 开启 AEC 的 MicroWakeWord 从处理后输出取帧，不再从此处喂麦克风帧。
        const size_t wake_samples = wake_running && !use_processed_wake
            ? wake_word_->GetFeedSize() : 0;
        const size_t processor_samples = audio_processor_->GetFeedSize();
        const size_t testing_samples = testing_running ? OPUS_FRAME_DURATION_MS * 16000 / 1000 : 0;
        size_t samples = 0;
        for (const size_t feed_size : {wake_samples, processor_samples, testing_samples}) {
            if (feed_size > 0 && (samples == 0 || feed_size < samples)) samples = feed_size;
        }
        if (samples == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        std::vector<int16_t> data;
        if (!ReadAudioData(data, 16000, samples)) {
            continue;
        }

        const size_t channels = codec_->input_channels();
        if (testing_samples > 0 && audio_testing_running_.load()) {
            for (size_t i = 0; i < data.size(); i += channels) {
                testing_pending.push_back(data[i]);
            }
            while (testing_pending.size() >= testing_samples) {
                std::vector<int16_t> frame(testing_pending.begin(), testing_pending.begin() + testing_samples);
                testing_pending.erase(testing_pending.begin(), testing_pending.begin() + testing_samples);
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(frame));
            }
        }

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

        if (wake_samples > 0 && IsWakeWordRunning()) {
#if CONFIG_USE_MICRO_WAKE_WORD
            if (channels == 2) {
                for (size_t i = 0; i < data.size(); i += 2) {
                    wake_pending.push_back(data[i]);
                }
            } else
#endif // CONFIG_USE_MICRO_WAKE_WORD
            {
                wake_pending.insert(wake_pending.end(), data.begin(), data.end());
            }
#if CONFIG_USE_MICRO_WAKE_WORD
            const size_t wake_channels = 1;
#else
            const size_t wake_channels = channels;
#endif
            const size_t wake_frame_size = wake_samples * wake_channels;
            while (wake_pending.size() >= wake_frame_size && IsWakeWordRunning()) {
                std::vector<int16_t> frame(wake_pending.begin(),
                                           wake_pending.begin() + wake_frame_size);
                wake_pending.erase(wake_pending.begin(),
                                   wake_pending.begin() + wake_frame_size);
                wake_word_->Feed(frame);
            }
        }
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

#if CONFIG_USE_MICRO_WAKE_WORD
// 只处理 AFE 后的最新唤醒帧；队列在 OnOutput 中限长，推理慢时丢旧帧。
void AudioService::ProcessedWakeTask() {
    while (true) {
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

        if (wake_uses_processor_output_.load() && IsWakeWordRunning()) {
            wake_word_->Feed(data);
        }
        // The streaming model can remain continuously runnable while AEC is
        // feeding it during TTS. Yield for one tick so the CPU0 application/UI
        // task and idle watchdog are not starved by wake-word inference.
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, "Processed wake task stopped");
}

void AudioService::ClearProcessedWakeQueue() {
    std::lock_guard<std::mutex> lock(processed_wake_mutex_);
    processed_wake_queue_.clear();
}

// 切换唤醒输入源时清掉旧源的缓存，并复位模型的连续输入状态。
void AudioService::SetWakeInputFromProcessor(bool enable) {
    const bool previous = wake_uses_processor_output_.exchange(enable);
    if (previous == enable) {
        return;
    }

    ClearProcessedWakeQueue();
    if (wake_word_initialized_ && IsWakeWordRunning()) {
        if (!static_cast<MicroWakeWord*>(wake_word_.get())->ResetInputState()) {
            ESP_LOGE(TAG, "Failed to reset wake word after changing input source");
        }
    }
    ESP_LOGI(TAG, "Wake word input source: %s",
             enable ? "AFE post-AEC output" : "amplified microphone PCM");
}
#endif

// DAC 的唯一写入者。服务端解码 PCM 与本地播放器 PCM 在此串行输出。
void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        if (service_stopped_) {
            break;
        }

        /* TTS starts with two seconds of actual PCM; other player sources use
           a small 4 KiB gate. After an underrun, resume on the first new PCM
           block. A completed stream always releases a tail below the target. */
        const bool is_player =
            audio_playback_queue_.front()->type == kAudioTaskTypePlayerToPlaybackQueue;
        const bool initial_prebuffer = player_output_chunks_.load() == 0;
        const size_t prebuffer_target = initial_prebuffer
                                            ? (player_tts_stream_
                                                   ? TTS_INITIAL_PREBUFFER_BYTES
                                                   : PLAYER_INITIAL_PREBUFFER_BYTES)
                                            : PLAYER_RECOVERY_PREBUFFER_BYTES;
        const auto queued_player_pcm_bytes = [this]() {
            size_t bytes = 0;
            for (const auto& queued_task : audio_playback_queue_) {
                if (queued_task->type != kAudioTaskTypePlayerToPlaybackQueue) {
                    break;
                }
                bytes += queued_task->PlaybackSamples() * sizeof(int16_t);
            }
            return bytes;
        };
        size_t prebuffered_pcm_bytes = is_player ? queued_player_pcm_bytes() : 0;
        while (!service_stopped_ && !audio_playback_queue_.empty() &&
               audio_playback_queue_.front()->type == kAudioTaskTypePlayerToPlaybackQueue &&
               player_prebuffering_ &&
               prebuffered_pcm_bytes < prebuffer_target &&
               !player_playback_finished_ &&
               is_player) {
            if (!player_prebuffer_wait_logged_) {
                player_prebuffer_wait_logged_ = true;
                ESP_LOGI(TAG, "播放器%s PCM 缓冲等待: bytes=%u/%u, queue=%u",
                         initial_prebuffer ? "首播" : "断流恢复",
                         static_cast<unsigned>(prebuffered_pcm_bytes),
                         static_cast<unsigned>(prebuffer_target),
                         static_cast<unsigned>(audio_playback_queue_.size()));
            }
            audio_queue_cv_.wait(lock);
            prebuffered_pcm_bytes = audio_playback_queue_.empty()
                                         ? 0
                                         : queued_player_pcm_bytes();
        }
        if (service_stopped_) {
            break;
        }
        if (audio_playback_queue_.empty()) {
            continue;
        }
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
        if (task->type == kAudioTaskTypePlayerToPlaybackQueue &&
            audio_playback_queue_.empty() && !player_playback_finished_) {
            player_prebuffering_ = true;
            player_prebuffer_wait_logged_ = false;
            ++player_rebuffer_count_;
            ESP_LOGW(TAG, "播放器队列欠载，等待下一块 PCM: recovery=%lu",
                     static_cast<unsigned long>(player_rebuffer_count_));
        }
        audio_output_busy_ = true;
        audio_queue_cv_.notify_all();
        lock.unlock();

        bool player_output_started = false;
        if (task->type == kAudioTaskTypePlayerToPlaybackQueue) {
            const uint32_t now_ms = esp_log_timestamp();
            /* Mark activity before the potentially blocking codec write. A
             * timeout callback can otherwise race with the first block after
             * a long prebuffer and mistake active output for silence. */
            player_output_last_activity_ms_.store(now_ms);
            player_output_started = player_output_chunks_.fetch_add(1) == 0;
        }

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }
        codec_->OutputData(task->PlaybackData(), task->PlaybackSamples());
        if (player_output_started && callbacks_.on_player_output_started) {
            ESP_LOGI(TAG, "播放器首个 PCM 已交给 codec: samples=%u",
                     static_cast<unsigned>(task->PlaybackSamples()));
            callbacks_.on_player_output_started(task->player_output);
        }
        if (task->type == kAudioTaskTypePlayerToPlaybackQueue) {
            const int sample_rate = codec_->output_sample_rate();
            const uint32_t duration_ms = sample_rate > 0
                ? static_cast<uint32_t>((static_cast<uint64_t>(task->PlaybackSamples()) * 1000) /
                                        static_cast<uint32_t>(sample_rate))
                : 0;
            if (duration_ms > 0) {
                const uint32_t audio_clock_ms =
                    player_audio_clock_ms_.fetch_add(duration_ms) + duration_ms;
                if (callbacks_.on_player_output_progress) {
                    auto output = task->player_output;
                    output.audio_clock_ms = audio_clock_ms;
                    callbacks_.on_player_output_progress(output);
                }
            }
        }

        lock.lock();
        audio_output_busy_ = false;
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (task->type == kAudioTaskTypePlayerToPlaybackQueue) {
            player_output_last_activity_ms_.store(esp_log_timestamp());
        }

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

// 同一工作任务兼顾下行 Opus 解码和上行 PCM 编码；两侧共享队列锁。
// 上行即使未开启网络上报仍编码，产物以有限长度的最新窗口保存。
void AudioService::OpusCodecTask() {
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
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
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
            } else if (opus_encoder_->sample_rate() != 16000) {
                int target_size = uplink_resampler_.GetOutputSamples(task->pcm.size());
                std::vector<int16_t> resampled(target_size);
                uplink_resampler_.Process(task->pcm.data(), task->pcm.size(), resampled.data());
                task->pcm = std::move(resampled);
                packet->sample_rate = opus_encoder_->sample_rate();
            }
            if (!raw_uplink && !opus_encoder_->Encode(std::move(task->pcm), packet->payload)) {
#else
            if (!opus_encoder_->Encode(std::move(task->pcm), packet->payload)) {
#endif // CONFIG_LSPLATFORM
                ESP_LOGE(TAG, "Failed to encode audio");
                continue;
            }

            if (task->type == kAudioTaskTypeEncodeToOpusQueue) {
                bool notify_consumer = false;
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    /* 开始上报时会推进 generation 并清空旧窗口。若一帧刚好
                     * 跨过这个边界，则丢弃它，避免把会话开始前的声音上报。 */
                    if (audio_upload_enabled_ &&
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
                if (notify_consumer && callbacks_.on_opus_packet_available) {
                    callbacks_.on_opus_packet_available();
                }
            } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                audio_testing_queue_.push_back(std::move(packet));
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

#ifdef CONFIG_LSPLATFORM
// 平台会话可选窄带上行；只改编码参数，不改 ADC/AFE 的 16 kHz 输入。
void AudioService::SetNarrowbandMode(bool enabled) {
    if (enabled) {
        SetEncodeSampleRate(8000, OPUS_FRAME_DURATION_MS);
    } else {
        SetEncodeSampleRate(16000, OPUS_FRAME_DURATION_MS);
    }
}

void AudioService::SetEncodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_encoder_->sample_rate() == sample_rate && opus_encoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_encoder_.reset();
    opus_encoder_ = std::make_unique<OpusWbEncoder>(sample_rate, 1, frame_duration);

    if (opus_encoder_->sample_rate() != 16000) {
        ESP_LOGI(TAG, "Resampling uplink audio to %d", opus_encoder_->sample_rate());
        uplink_resampler_.Configure(16000, opus_encoder_->sample_rate());
    }
}
#endif // CONFIG_LSPLATFORM

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

// AFE 的实时帧满队列时丢旧帧，不阻塞采集；音频测试帧则允许等待空位。
void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);

    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
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
        }
        if (!dropped_queued_frame) {
            return;
        }
    }
    if (type == kAudioTaskTypeEncodeToOpusQueue) {
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
bool AudioService::PushPcmToPlaybackQueue(const uint8_t* data, size_t size,
                                          int sample_rate,
                                          const PlaybackOutputEvent& output,
                                          bool wait) {
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
    const bool queue_was_full = audio_playback_queue_.size() >= MAX_PLAYER_PLAYBACK_TASKS_IN_QUEUE;
    if (wait) {
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ || audio_playback_queue_.size() < MAX_PLAYER_PLAYBACK_TASKS_IN_QUEUE;
        });
        const uint32_t enqueue_wait_ms = esp_log_timestamp() - enqueue_wait_start_ms;
        if (queue_was_full && enqueue_wait_ms >= 80) {
            ESP_LOGI(TAG, "播放器生产受队列反压: wait=%lums, queue=%u",
                     static_cast<unsigned long>(enqueue_wait_ms),
                     static_cast<unsigned>(audio_playback_queue_.size()));
        }
    } else if (audio_playback_queue_.size() >= MAX_PLAYER_PLAYBACK_TASKS_IN_QUEUE) {
        return false;
    }

    if (service_stopped_) {
        return false;
    }
    audio_playback_queue_.push_back(std::move(task));
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

// 新播放器会话重置下行解码/缓冲状态；stream_active 防止等待首帧时关闭 DAC。
void AudioService::BeginPlayerPlayback(const PlaybackOutputEvent& output) {
    ResetDecoder();
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        player_tts_stream_ = output.channel == PlaybackChannel::kTts;
    }
    player_stream_active_.store(true);
}

// 标记 PCM 已全部入队，使短尾帧越过预缓冲门槛，并补齐最后一个 DMA 缓冲。
void AudioService::MarkPlayerPlaybackFinished(
        const PlaybackOutputEvent& output) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
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
        player_pcm_bytes_ += padding_bytes;
        ESP_LOGI(TAG, "播放器末尾 DMA 对齐: padding=%u bytes",
                 static_cast<unsigned>(padding_bytes));
    }
    audio_queue_cv_.notify_all();
}

// 立即撤销待播放 PCM；正在执行的 codec 写入由输出任务自行结束。
void AudioService::ClearPlaybackQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    player_stream_active_.store(false);
    audio_playback_queue_.clear();
    audio_queue_cv_.notify_all();
}

// 等到播放队列和当前 codec 写入结束，再等待 I2S DMA 环里的尾音实际送出。
void AudioService::WaitForPlaybackComplete() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() {
        return service_stopped_ || (audio_playback_queue_.empty() && !audio_output_busy_);
    });
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

// 网络消费入口：关闭上报时不取包，后台编码仍继续更新最新窗口。
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

// 将唤醒模型保留的唤醒词录音编码，供后续 PopWakeWordPacket() 读取。
void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
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

// 读取唤醒词预录音，与常规上行 opus_packet_queue_ 相互独立。
std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    if (!wake_word_) {
        return nullptr;
    }
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        packet->sample_rate = 16000;
        packet->frame_duration = OPUS_FRAME_DURATION_MS;
        return packet;
    }
    return nullptr;
}

// 只控制唤醒模型的运行与喂帧，不停止 ADC、AFE 或编码任务。
void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }
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
    wake_word_running_.store(false);
    ClearProcessedWakeQueue();
    static_cast<MicroWakeWord*>(wake_word_.get())->Pause();
#else
    EnableWakeWordDetection(false);
#endif
}

// 上报会话边界：开启时清掉此前缓存，并用 generation 排除正在编码的旧帧。
// 关闭时只停止通知/取包；采集、AFE 和 Opus 编码继续运行。
void AudioService::EnableAudioUpload(bool enable) {
    bool previous = false;
    size_t encode_before = 0;
    size_t opus_before = 0;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        previous = audio_upload_enabled_;
        if (previous == enable) {
            return;
        }
        encode_before = audio_encode_queue_.size();
        opus_before = opus_packet_queue_.size();
        audio_upload_enabled_ = enable;
        generation = ++upload_generation_;
        if (enable) {
            /* 离线期间仍持续编码并保存最新窗口；新会话从此刻开始，不能把
             * 窗口里的历史声音发给服务端。 */
            audio_encode_queue_.clear();
            opus_packet_queue_.clear();
        }
        audio_queue_cv_.notify_all();
    }
    ESP_LOGI(TAG,
             "音频上报: %d->%d, generation=%lu, 开始上报时清理=pcm:%u/opus:%u, "
             "处理链常驻=%d",
             previous ? 1 : 0, enable ? 1 : 0,
             static_cast<unsigned long>(generation),
             static_cast<unsigned>(enable ? encode_before : 0),
             static_cast<unsigned>(enable ? opus_before : 0),
             service_stopped_.load() ? 0 : 1);
}

// AEC 同时要求编译支持、codec 全双工以及 ADC 参考声道。
bool AudioService::SupportsAec() const {
#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_AUDIO_PROCESSOR
    return codec_ && codec_->duplex() && codec_->input_reference();
#else
    return false;
#endif
}

// 录音测试从增益处理后的麦克风取帧，停止时把测试 Opus 送去回放。
void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        audio_testing_running_.store(true);
    } else {
        audio_testing_running_.store(false);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

// 根据硬件能力切换 AEC；MicroWakeWord 随之改用处理后或原始麦克风输入。
void AudioService::SetAecEnabled(bool enable) {
    aec_enabled_ = enable && SupportsAec();
    if (enable && !aec_enabled_) {
        ESP_LOGW(TAG, "当前板型不支持 AEC，保持关闭");
    }
    if (audio_processor_initialized_) {
        audio_processor_->SetAecEnabled(aec_enabled_);
#if CONFIG_USE_MICRO_WAKE_WORD
        if (!service_stopped_.load()) {
            SetWakeInputFromProcessor(aec_enabled_);
        }
#endif
    }
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
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

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

// 清理一次下行播放会话的解码器、重采样器、播放队列和首帧统计。
void AudioService::ResetDecoder() {
    std::scoped_lock lock(player_resampler_mutex_, audio_queue_mutex_);
    opus_decoder_->ResetState();
    /* 新播放会话以干净的重采样状态开始，避免上一段音频的残留相位 */
    player_resampler_input_rate_ = 0;
    player_output_chunks_.store(0);
    player_output_last_activity_ms_.store(0);
    player_audio_clock_ms_.store(0);
    player_pcm_bytes_ = 0;
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    player_playback_finished_ = false;
    player_tts_stream_ = false;
    player_prebuffering_ = true;
    player_prebuffer_wait_logged_ = false;
    player_rebuffer_count_ = 0;
    player_stream_active_.store(false);
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
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
        ParseMicroWakeWordConfig(descriptor, config);

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
