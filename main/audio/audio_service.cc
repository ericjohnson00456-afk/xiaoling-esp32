#include "audio_service.h"
#include "xiaoling/conn/xiaoling_config.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <algorithm>
#include <climits>
#include <cstring>

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

namespace {
constexpr UBaseType_t kAudioCodecPriority = 11;
constexpr UBaseType_t kAudioAlgorithmPriority = 8;
constexpr uint32_t kAudioInputIdleYieldIntervalMs = 500;
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


AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}


void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    /* Setup the audio codec */
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusWbEncoder>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        uplink_afe_frames_.fetch_add(1);
        uplink_last_afe_ms_.store(esp_log_timestamp());
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
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

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

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

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

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
#if CONFIG_USE_MICRO_WAKE_WORD
    ClearProcessedWakeQueue();
    processed_wake_cv_.notify_all();
#endif
}

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
    debug_statistics_.input_count++;
    uplink_mic_reads_.fetch_add(1);
    uplink_last_mic_ms_.store(esp_log_timestamp());

    return true;
}

void AudioService::AudioInputTask() {
    std::vector<int16_t> wake_pending;
    std::vector<int16_t> processor_pending;
    std::vector<int16_t> testing_pending;
    TickType_t last_idle_yield_tick = xTaskGetTickCount();
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }

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

        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        const bool testing_running = (bits & AS_EVENT_AUDIO_TESTING_RUNNING) != 0;
        if (testing_running) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
        } else testing_pending.clear();

        const bool wake_running = (bits & AS_EVENT_WAKE_WORD_RUNNING) != 0;
        const bool processor_running = (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) != 0;
#if CONFIG_USE_MICRO_WAKE_WORD
        const bool use_processed_wake = wake_uses_processor_output_.load() && processor_running;
#else
        const bool use_processed_wake = false;
#endif
        if (!wake_running) wake_pending.clear();
        if (use_processed_wake) wake_pending.clear();
        if (!processor_running) processor_pending.clear();

        const size_t wake_samples = wake_running && !use_processed_wake
            ? wake_word_->GetFeedSize() : 0;
        const size_t processor_samples = processor_running ? audio_processor_->GetFeedSize() : 0;
        const size_t testing_samples = testing_running ? OPUS_FRAME_DURATION_MS * 16000 / 1000 : 0;
        size_t samples = 0;
        for (const size_t feed_size : {wake_samples, processor_samples, testing_samples}) {
            if (feed_size > 0 && (samples == 0 || feed_size < samples)) samples = feed_size;
        }
        if (samples > 0) {
            std::vector<int16_t> data;
            if (!ReadAudioData(data, 16000, samples)) {
                continue;
            }

            const size_t channels = codec_->input_channels();
            if (testing_samples > 0 && (xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_TESTING_RUNNING)) {
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
                const size_t frame_size = processor_samples * channels;
                while (processor_pending.size() >= frame_size && IsAudioProcessorRunning()) {
                    std::vector<int16_t> frame(processor_pending.begin(), processor_pending.begin() + frame_size);
                    processor_pending.erase(processor_pending.begin(), processor_pending.begin() + frame_size);
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
                const size_t frame_size = wake_samples * wake_channels;
                while (wake_pending.size() >= frame_size && IsWakeWordRunning()) {
                    std::vector<int16_t> frame(wake_pending.begin(), wake_pending.begin() + frame_size);
                    wake_pending.erase(wake_pending.begin(), wake_pending.begin() + frame_size);
#if CONFIG_USE_AUDIO_DEBUGGER
                    std::lock_guard<std::mutex> debug_lock(audio_debugger_mutex_);
                    if (!audio_debugger_) {
                        audio_debugger_ = std::make_unique<AudioDebugger>();
                    }
                    audio_debugger_->Feed(frame);
#endif
                    wake_word_->Feed(frame);
                }
            }
            continue;
        }

        /* A module can stop between reading the event bits and its feed size. */
        ESP_LOGW(TAG, "Stale audio input event bits: %lx, clearing", bits);
        xEventGroupClearBits(event_group_, bits & (AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

#if CONFIG_USE_MICRO_WAKE_WORD
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
#if CONFIG_USE_AUDIO_DEBUGGER
            std::lock_guard<std::mutex> debug_lock(audio_debugger_mutex_);
            if (!audio_debugger_) {
                audio_debugger_ = std::make_unique<AudioDebugger>();
            }
            audio_debugger_->Feed(data);
#endif
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
        const size_t queue_size_after_pop = audio_playback_queue_.size();
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
            const uint32_t previous_end_ms = player_output_last_end_ms_.load();
            /* Mark activity before the potentially blocking codec write. A
             * timeout callback can otherwise race with the first block after
             * a long prebuffer and mistake active output for silence. */
            player_output_last_activity_ms_.store(now_ms);
            if (previous_end_ms != 0) {
                const uint32_t gap_ms = now_ms - previous_end_ms;
                if (gap_ms >= 80) {
                    const uint32_t underruns = player_output_underruns_.fetch_add(1) + 1;
                    ESP_LOGW(TAG, "播放器输出出现间隙: gap=%lums, underruns=%lu, queue_before=%u",
                             static_cast<unsigned long>(gap_ms),
                             static_cast<unsigned long>(underruns),
                             static_cast<unsigned>(queue_size_after_pop));
                }
            }
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
            callbacks_.on_player_output_started();
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
                    callbacks_.on_player_output_progress(audio_clock_ms);
                }
            }
        }

        lock.lock();
        audio_output_busy_ = false;
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (task->type == kAudioTaskTypePlayerToPlaybackQueue) {
            const uint32_t output_end_ms = esp_log_timestamp();
            player_output_last_end_ms_.store(output_end_ms);
            player_output_last_activity_ms_.store(output_end_ms);
        }

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            lock.lock();
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
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
            task->timestamp = packet->timestamp;

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
            debug_statistics_.decode_count++;
        }
        
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                uplink_encode_dequeued_.fetch_add(1);
            }
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;
#ifdef CONFIG_LSPLATFORM
            const bool raw_uplink = task->type == kAudioTaskTypeEncodeToSendQueue &&
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

            if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    if (uplink_enabled_ && task->uplink_generation == uplink_generation_) {
                        audio_send_queue_.push_back(std::move(packet));
                        uplink_encoded_packets_.fetch_add(1);
                        uplink_last_encoded_ms_.store(esp_log_timestamp());
                    }
                }
                if (callbacks_.on_send_queue_available) {
                    uplink_send_notifications_.fetch_add(1);
                    uplink_last_notification_ms_.store(esp_log_timestamp());
                    callbacks_.on_send_queue_available();
                }
            } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                audio_testing_queue_.push_back(std::move(packet));
            }
            debug_statistics_.encode_count++;
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

#ifdef CONFIG_LSPLATFORM
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

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (type == kAudioTaskTypeEncodeToSendQueue && !uplink_enabled_) return;
    task->uplink_generation = uplink_generation_;

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    if (type == kAudioTaskTypeEncodeToSendQueue &&
        audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
        // Uplink audio is real-time data. Blocking the AFE callback here makes
        // its input ringbuffer overflow and breaks barge-in. Dropping one old
        // frame is preferable to stalling the microphone pipeline.
        audio_encode_queue_.pop_front();
        const uint32_t dropped_uplink_frames =
            uplink_dropped_encode_frames_.fetch_add(1) + 1;
        if ((dropped_uplink_frames % 25) == 1) {
            ESP_LOGW(TAG,
                     "上行编码队列满，丢弃旧帧: count=%lu, encode_q=%u, send_q=%u, "
                     "afe=%lu, encoded=%lu, popped=%lu",
                     static_cast<unsigned long>(dropped_uplink_frames),
                     static_cast<unsigned>(audio_encode_queue_.size()),
                     static_cast<unsigned>(audio_send_queue_.size()),
                     static_cast<unsigned long>(uplink_afe_frames_.load()),
                     static_cast<unsigned long>(uplink_encoded_packets_.load()),
                     static_cast<unsigned long>(uplink_popped_packets_.load()));
        }
    }
    audio_queue_cv_.wait(lock, [this, &task]() {
        return service_stopped_ || task->uplink_generation != uplink_generation_ ||
               audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE;
    });
    if (service_stopped_ || (type == kAudioTaskTypeEncodeToSendQueue &&
        (!uplink_enabled_ || task->uplink_generation != uplink_generation_))) return;
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPcmToPlaybackQueue(const uint8_t* data, size_t size, int sample_rate, bool wait) {
    if (data == nullptr || size == 0 || (size % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "无效的播放器 PCM 数据: data=%p, bytes=%u", data,
                 static_cast<unsigned>(size));
        return false;
    }

    auto task = std::make_unique<AudioTask>();
    task->type = kAudioTaskTypePlayerToPlaybackQueue;
    task->timestamp = 0;
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

void AudioService::BeginPlayerPlayback(bool tts_stream) {
    ResetDecoder();
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        player_tts_stream_ = tts_stream;
    }
    player_stream_active_.store(true);
}

void AudioService::MarkPlayerPlaybackFinished() {
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

void AudioService::ClearPlaybackQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    player_stream_active_.store(false);
    audio_playback_queue_.clear();
    audio_queue_cv_.notify_all();
}

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

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    uplink_popped_packets_.fetch_add(1);
    uplink_last_popped_ms_.store(esp_log_timestamp());
    audio_queue_cv_.notify_all();
    return packet;
}

AudioUplinkDiagnostics AudioService::GetUplinkDiagnostics() {
    AudioUplinkDiagnostics diagnostics;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        diagnostics.upload_enabled = uplink_enabled_;
        diagnostics.generation = uplink_generation_;
        diagnostics.encode_queue = audio_encode_queue_.size();
        diagnostics.send_queue = audio_send_queue_.size();
    }
    diagnostics.processor_running = IsAudioProcessorRunning();
    diagnostics.mic_reads = uplink_mic_reads_.load();
    diagnostics.afe_frames = uplink_afe_frames_.load();
    diagnostics.encode_dequeued = uplink_encode_dequeued_.load();
    diagnostics.encoded_packets = uplink_encoded_packets_.load();
    diagnostics.send_notifications = uplink_send_notifications_.load();
    diagnostics.popped_packets = uplink_popped_packets_.load();
    diagnostics.dropped_encode_frames = uplink_dropped_encode_frames_.load();
    diagnostics.last_mic_ms = uplink_last_mic_ms_.load();
    diagnostics.last_afe_ms = uplink_last_afe_ms_.load();
    diagnostics.last_encoded_ms = uplink_last_encoded_ms_.load();
    diagnostics.last_notification_ms = uplink_last_notification_ms_.load();
    diagnostics.last_popped_ms = uplink_last_popped_ms_.load();
    return diagnostics;
}

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
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        /* 先清除事件位再 Stop，缩小输入任务读到残留 RUNNING 位的窗口 */
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
#if CONFIG_USE_MICRO_WAKE_WORD
        ClearProcessedWakeQueue();
#endif
        wake_word_->Stop();
    }
}

void AudioService::PauseWakeWordDetection() {
#if CONFIG_USE_MICRO_WAKE_WORD
    if (!wake_word_) {
        return;
    }
    xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    ClearProcessedWakeQueue();
    static_cast<MicroWakeWord*>(wake_word_.get())->Pause();
#else
    EnableWakeWordDetection(false);
#endif
}

void AudioService::EnableAudioUpload(bool enable) {
    bool previous = false;
    size_t encode_before = 0;
    size_t send_before = 0;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        previous = uplink_enabled_;
        encode_before = audio_encode_queue_.size();
        send_before = audio_send_queue_.size();
        uplink_enabled_ = enable;
        generation = ++uplink_generation_;
        if (!enable) {
            audio_encode_queue_.clear();
            audio_send_queue_.clear();
            timestamp_queue_.clear();
        }
        audio_queue_cv_.notify_all();
    }
    ESP_LOGI(TAG,
             "上行链路[本地开关]: %d->%d, generation=%lu, cleared=encode:%u/send:%u, "
             "processor=%d",
             previous ? 1 : 0, enable ? 1 : 0,
             static_cast<unsigned long>(generation),
             static_cast<unsigned>(enable ? 0 : encode_before),
             static_cast<unsigned>(enable ? 0 : send_before),
             IsAudioProcessorRunning() ? 1 : 0);
}

void AudioService::EnableVoiceProcessing(bool enable) {
    EnableAudioUpload(enable);
    if (enable == IsAudioProcessorRunning()) return;
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }

        // Playback belongs to AudioPlayer and may run concurrently in full duplex.
        audio_input_need_warmup_ = !IsWakeWordRunning();
        audio_processor_->EnableDeviceAec(device_aec_requested_);
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
#if CONFIG_USE_MICRO_WAKE_WORD
        SetWakeInputFromProcessor(device_aec_requested_ && SupportsDeviceAec());
#endif
    } else {
#if CONFIG_USE_MICRO_WAKE_WORD
        SetWakeInputFromProcessor(false);
#endif
        /* 先清除事件位再 Stop，缩小输入任务读到残留 RUNNING 位的窗口 */
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        audio_processor_->Stop();
        audio_processor_->EnableDeviceAec(false);
    }
}

bool AudioService::SupportsDeviceAec() const {
#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_AUDIO_PROCESSOR
    return codec_ && codec_->duplex() && codec_->input_reference();
#else
    return false;
#endif
}

bool AudioService::SupportsAec() const {
#if CONFIG_USE_DEVICE_AEC || CONFIG_USE_SERVER_AEC
    return SupportsDeviceAec() ||
#if CONFIG_USE_SERVER_AEC
           true;
#else
           false;
#endif
#else
    return false;
#endif
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    device_aec_requested_ = enable;
    if (audio_processor_initialized_ && IsAudioProcessorRunning()) {
        audio_processor_->EnableDeviceAec(enable);
#if CONFIG_USE_MICRO_WAKE_WORD
        SetWakeInputFromProcessor(enable && SupportsDeviceAec());
#endif
    }
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

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

void AudioService::ResetDecoder() {
    std::scoped_lock lock(player_resampler_mutex_, audio_queue_mutex_);
    opus_decoder_->ResetState();
    /* 新播放会话以干净的重采样状态开始，避免上一段音频的残留相位 */
    player_resampler_input_rate_ = 0;
    player_output_chunks_.store(0);
    player_output_last_end_ms_.store(0);
    player_output_last_activity_ms_.store(0);
    player_output_underruns_.store(0);
    player_audio_clock_ms_.store(0);
    player_pcm_bytes_ = 0;
    timestamp_queue_.clear();
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

#if CONFIG_USE_MICRO_WAKE_WORD
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
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        if (callbacks_.on_wake_word_detected) {
            callbacks_.on_wake_word_detected(wake_word);
        }
    });
}
#else // !CONFIG_USE_MICRO_WAKE_WORD
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
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
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
