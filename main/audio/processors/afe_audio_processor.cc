#include "afe_audio_processor.h"
#include "speech_agc_guard.h"
#include "audio/audio_yield_policy.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <algorithm>
#include <cmath>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    // AEC 需要优先保证回声消除效果，使用 ESP-SR 的高性能全双工配置。
    // 采集任务的调度让出和唤醒任务的核心隔离负责避免 DSP 长时间独占 CPU。
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
#if CONFIG_SR_NSN_WEBRTC
        // 此版本 VC AFE 的 WebRTC NS 初始化会解引用空接口。使用独立的
        // 10 ms NS 放在 AEC 后，避免非线性降噪影响回声滤波器收敛。
        output_ns_ = ns_pro_create(10, 1, 16000);
        ESP_LOGI(TAG, "AEC 后置 WebRTC NS: %s, mode=medium, frame=10ms",
                 output_ns_ != nullptr ? "enabled" : "allocation failed");
#endif
    }

    // AFE 内部 AGC 保持关闭，独立 AGC 位于 NS 后面。
    afe_config->agc_init = false;
#if CONFIG_IDF_TARGET_ESP32S3
    gain_vad_ = vad_create_with_param(VAD_MODE_1, 16000, 20, 60, 200);
    output_agc_ = esp_agc_open(AGC_MODE_2, 16000);
    if (gain_vad_ != nullptr && output_agc_ != nullptr) {
        // 数字自适应增益，目标包络 -6 dBFS，内部 limiter 开启。
        // 输出还会限制相对 NS PCM 的额外增益，并在非人声时旁路。
        set_agc_config(output_agc_, 18, 1, 6);
        ESP_LOGI(TAG, "NS 后人声 AGC: enabled, extra_gain<=12dB, limiter=on");
    } else {
        if (gain_vad_ != nullptr) vad_destroy(gain_vad_);
        if (output_agc_ != nullptr) esp_agc_close(output_agc_);
        gain_vad_ = nullptr;
        output_agc_ = nullptr;
        ESP_LOGW(TAG, "人声 AGC 初始化失败，保留 AEC/NS 输出");
    }
#endif
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = kTaskPriority;

#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    // Both modules must be created because the application can switch
    // between full-duplex AEC and half-duplex VAD at runtime.
    afe_config->vad_init = true;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    
    // 后置 NS 需要额外 DSP 栈；优先放入 PSRAM，保留内部 SRAM 给 DMA/网络。
    const uint32_t stack_bytes =
        (output_ns_ != nullptr || output_agc_ != nullptr) ? 16384 : 4096;
    const BaseType_t result = xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDeleteWithCaps(nullptr);
    }, "audio_communication", stack_bytes, this, kTaskPriority, nullptr, kTaskCore,
       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != pdPASS) {
        ESP_LOGW(TAG, "AFE/NS PSRAM 栈分配失败，回退内部 SRAM");
        xTaskCreatePinnedToCore([](void* arg) {
            static_cast<AfeAudioProcessor*>(arg)->AudioProcessorTask();
            vTaskDelete(nullptr);
        }, "audio_communication", stack_bytes, this, kTaskPriority, nullptr, kTaskCore);
    }
    ESP_LOGI(TAG, "AFE/NS 取帧任务栈: %lu bytes", static_cast<unsigned long>(stack_bytes));
#if CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, "ESP32-S3 处理顺序: MIC 前置增益 -> AEC(若启用) -> NS -> 人声自适应 AGC");
#endif
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (gain_vad_ != nullptr) vad_destroy(gain_vad_);
    if (output_agc_ != nullptr) esp_agc_close(output_agc_);
    if (output_ns_ != nullptr) ns_destroy(output_ns_);
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::EmitOutput(std::vector<int16_t>&& data) {
    constexpr size_t kNsFrameSamples = 160;
    if (output_ns_ != nullptr && data.size() % kNsFrameSamples == 0) {
        std::vector<int16_t> cleaned(data.size());
        for (size_t offset = 0; offset < data.size(); offset += kNsFrameSamples) {
            ns_process(output_ns_, data.data() + offset, cleaned.data() + offset);
        }
        data = std::move(cleaned);
    }
    ApplySpeechAgc(data);
    output_callback_(std::move(data));
}

void AfeAudioProcessor::ApplySpeechAgc(std::vector<int16_t>& data) {
    constexpr size_t kFrameSamples = 320;
    constexpr size_t kAgcFrameSamples = 160;
    if (output_agc_ == nullptr || gain_vad_ == nullptr ||
        data.size() != kFrameSamples) return;

    const uint64_t input_power = SpeechAgcGuard::IntegerPower(data);
    const bool speech =
        vad_process_with_trigger(gain_vad_, data.data()) == VAD_SPEECH &&
        input_power >= 64ULL * 64 * data.size(); // 保留原 RMS>=64 门控。

    std::vector<int16_t> gained(data.size());
    for (size_t offset = 0; offset < data.size(); offset += kAgcFrameSamples) {
        const int result = esp_agc_process(
            output_agc_, data.data() + offset, gained.data() + offset,
            kAgcFrameSamples, 16000);
        if (result < 0) {
            ESP_LOGW(TAG, "后置 AGC 处理失败: %d，保留 NS PCM", result);
            return;
        }
    }

    const float previous_mix = agc_mix_;
    // VAD 已有 200 ms 尾音保持；非人声/极低电平帧在一帧内平滑回到
    // 旁路，不再叠加慢回落，避免停顿段拖着高增益。
    if (speech) {
        agc_mix_ += (1.0f - agc_mix_) * 0.25f;
    } else {
        agc_mix_ = 0.0f;
    }
    SpeechAgcGuard::Apply(data, gained, previous_mix, agc_mix_);
    if (++agc_frames_ % 300 == 0) {
        const double input_rms = std::sqrt(static_cast<double>(input_power) / data.size());
        const double output_rms = std::sqrt(SpeechAgcGuard::Power(data) / data.size());
        const double input_db = input_rms > 0.0
            ? 20.0 * std::log10(input_rms / 32768.0) : -120.0;
        const double output_db = output_rms > 0.0
            ? 20.0 * std::log10(output_rms / 32768.0) : -120.0;
        ESP_LOGI(TAG,
                 "人声 AGC: speech=%d, NS=%.1fdBFS, out=%.1fdBFS, "
                 "gain=%.1fdB, mix=%.2f",
                 speech ? 1 : 0, input_db, output_db,
                 output_db - input_db, static_cast<double>(agc_mix_));
    }
}

void AfeAudioProcessor::AudioProcessorTask() {
    size_t processed_frames = 0;
    int64_t stats_started_us = esp_timer_get_time();
    uint32_t output_frames = 0;
    int64_t processing_us = 0, max_processing_us = 0;
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }
        // 在 fetch 返回后检查，避免阻塞等待跨过共同让步窗口。
        if (const auto ticks = AudioYieldDelayTicks(xTaskGetTickCount(), pdMS_TO_TICKS(100)); ticks) {
            vTaskDelay(ticks);
        }
        if (++processed_frames % 300 == 0) {
            ESP_LOGI(TAG, "AFE/NS 最小剩余栈: %u bytes",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        }

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (output_callback_) {
            const int64_t processing_started_us = esp_timer_get_time();
            size_t samples = res->data_size / sizeof(int16_t);
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            
            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    EmitOutput(std::move(output_buffer_));
                    ++output_frames;
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    EmitOutput(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    ++output_frames;
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
            const int64_t elapsed_us = esp_timer_get_time() - processing_started_us;
            processing_us += elapsed_us;
            max_processing_us = std::max(max_processing_us, elapsed_us);
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us - stats_started_us >= 5 * 1000 * 1000) {
            ESP_LOGI(TAG, "AFE 后处理: output=%.1ffps, avg=%.2fms, max=%.2fms",
                     output_frames * 1000000.0 / (now_us - stats_started_us),
                     output_frames ? processing_us / (output_frames * 1000.0) : 0.0,
                     max_processing_us / 1000.0);
            stats_started_us = now_us;
            output_frames = 0;
            processing_us = max_processing_us = 0;
        }
    }
}

void AfeAudioProcessor::SetAecEnabled(bool enable) {
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "当前板型未启用 AEC");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}
