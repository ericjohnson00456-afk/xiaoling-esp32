#include "micro_wake_word.h"
#include "audio/wake_audio_recorder.h"
#include "micro_wake_word/model_wake_word.h"
#include "micro_wake_word/streaming_model.h"
#include "micro_wake_word/preprocessor_settings.h"
#include "model_vad.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_spiffs.h>
#include <spi_flash_mmap.h>
#include <frontend.h>
#include <frontend_util.h>
#include <cstring>
#include <algorithm>

#define TAG "MicroWakeWord"

extern const uint8_t vad_model_data[] asm("_binary_vad_tflite_start");

MicroWakeWord::MicroWakeWord(const void* model_data, size_t model_size,
                             const MicroWakeWordConfig& config)
    : model_data_(model_data), model_size_(model_size), config_(config) {
    // Initialize ring buffer
    ring_buffer_size_ = BUFFER_SIZE;
    ring_buffer_.resize(ring_buffer_size_);
    
    // Initialize frontend config - will be set properly in Initialize()
    memset(&frontend_config_, 0, sizeof(frontend_config_));
    memset(&frontend_state_, 0, sizeof(frontend_state_));
}

MicroWakeWord::~MicroWakeWord() {
    Stop();
}

bool MicroWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    if (initialized_) {
        return true;
    }

    codec_ = codec;

    if (config_.feature_step_size == 0 || config_.feature_step_size * 16 >= BUFFER_SIZE) {
        ESP_LOGE(TAG, "Invalid feature step size: %u", config_.feature_step_size);
        return false;
    }
    if (config_.feature_step_size != micro_wake_word::kVadFeatureStepMs) {
        ESP_LOGE(TAG, "Wake/VAD feature steps differ; shared frontend unavailable");
        return false;
    }
    
    if (!RegisterStreamingOps(streaming_op_resolver_)) {
        ESP_LOGE(TAG, "Failed to register streaming operations");
        return false;
    }

    // Configure audio frontend
    frontend_config_.window.size_ms = FEATURE_DURATION_MS;
    features_step_size_ = config_.feature_step_size;
    frontend_config_.window.step_size_ms = features_step_size_;
    frontend_config_.filterbank.num_channels = PREPROCESSOR_FEATURE_SIZE;
    frontend_config_.filterbank.lower_band_limit = 125.0;
    frontend_config_.filterbank.upper_band_limit = 7500.0;
    frontend_config_.noise_reduction.smoothing_bits = 10;
    frontend_config_.noise_reduction.even_smoothing = 0.025;
    frontend_config_.noise_reduction.odd_smoothing = 0.06;
    frontend_config_.noise_reduction.min_signal_remaining = 0.05;
    frontend_config_.pcan_gain_control.enable_pcan = 1;
    frontend_config_.pcan_gain_control.strength = 0.95;
    frontend_config_.pcan_gain_control.offset = 80.0;
    frontend_config_.pcan_gain_control.gain_bits = 21;
    frontend_config_.log_scale.enable_log = 1;
    frontend_config_.log_scale.scale_shift = 6;
    
    ESP_LOGI(TAG, "Frontend config: window_size=%dms, step_size=%dms, channels=%d, sample_rate=%d", 
            frontend_config_.window.size_ms, frontend_config_.window.step_size_ms,
            frontend_config_.filterbank.num_channels, AUDIO_SAMPLE_FREQUENCY);

    // Clean up any existing frontend state safely
    if (frontend_initialized_) {
        FrontendFreeStateContents(&frontend_state_);
        frontend_initialized_ = false;
    }
    memset(&frontend_state_, 0, sizeof(frontend_state_));
    
    // Setup audio frontend
    if (!FrontendPopulateState(&frontend_config_, &frontend_state_, AUDIO_SAMPLE_FREQUENCY)) {
        ESP_LOGE(TAG, "Failed to populate frontend state");
        return false;
    }
    frontend_initialized_ = true;

    ESP_LOGI(TAG, "Loading wake word model from %p", model_data_);

    auto model = std::make_unique<micro_wake_word::WakeWordModel>(
        (const uint8_t *)model_data_,
        config_.probability_cutoff,
        config_.sliding_window_size,
        config_.wake_word.empty() ? kMicroWakeWordModelWakeWord : config_.wake_word,
        config_.tensor_arena_size);

    wake_word_models_.push_back(std::move(model));
    vad_model_ = std::make_unique<micro_wake_word::VADModel>(
        vad_model_data, micro_wake_word::kVadProbabilityCutoff,
        micro_wake_word::kVadWindowSize, micro_wake_word::kVadTensorArenaSize);

    initialized_ = true;
    ESP_LOGI(TAG, "MicroWakeWord initialized successfully");
    return true;
}

void MicroWakeWord::Feed(const std::vector<int16_t>& data) {
    // 与 Stop() 互斥：推理期间模型/arena 不会被卸载。
    // 注意：detection_callback_ 只设置事件位（见 Application::Start），
    // 不会同步回调进 MicroWakeWord，持锁调用不会死锁。
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || data.empty()) {
        return;
    }

#ifdef CONFIG_MICRO_WAKE_WORD_DEBUG
    // 调试：检查输入数据（只在调试模式下）
    static int feed_count = 0;
    if (feed_count < 3) {
        bool all_zero = std::all_of(data.begin(), data.end(), [](int16_t val) { return val == 0; });
        auto [min_val, max_val] = std::minmax_element(data.begin(), data.end());
        ESP_LOGI(TAG, "Feed #%d: size=%d, all_zero=%s, range=[%d,%d]", 
                feed_count++, data.size(), all_zero ? "yes" : "no", *min_val, *max_val);
    }
#endif

    // Bound each write so large Feed calls cannot overwrite unconsumed PCM.
    for (size_t offset = 0; offset < data.size();) {
        const size_t count = std::min({data.size() - offset, size_t(GetNewSamplesToGet()),
                                       ring_buffer_size_ - 1 - GetRingBufferAvailable()});
        WriteToRingBuffer(data.data() + offset, count);
        offset += count;

        while (HasEnoughSamples()) {
            const bool capturing = wake_score_capture_active_.load();
            const bool updated = UpdateModelProbabilities();
            if (!running_) return;
            if (!updated) continue;

            if (capturing) {
                const bool pending = std::any_of(
                    wake_word_models_.begin(), wake_word_models_.end(),
                    [](const auto& model) { return model->IsWakeScoreCaptureActive(); });
                if (!pending) {
                    // 后组采满后才执行原有复位，避免把新会话的分数混入本轮日志。
                    wake_score_capture_active_ = false;
                    if (!ResetInputStateLocked()) {
                        ESP_LOGE(TAG, "Wake score capture completed, model reset failed");
                        running_ = false;
                        return;
                    } else {
                        ESP_LOGI(TAG, "Wake score capture completed; wake model reset after AFTER20");
                    }
                }
                continue;
            }

            if (DetectWakeWords()) {
                last_detected_wake_word_ = detected_wake_word_;
                detected_ = true;
                wake_score_capture_active_ = true;

                if (detection_callback_) {
                    detection_callback_(detected_wake_word_);
                }

                // 保持流式状态收集后续 20 次评分，再恢复原有复位行为。
                detected_wake_word_.clear();
                detected_ = false;
            }
        }
    }
}

void MicroWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    detection_callback_ = callback;
}

void MicroWakeWord::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        ESP_LOGE(TAG, "MicroWakeWord not initialized");
        return;
    }

    if (running_) {
        ESP_LOGW(TAG, "MicroWakeWord already running");
        return;
    }

    if (models_loaded_) {
        if (!frontend_initialized_) {
            UnloadModels();
        } else {
            FrontendReset(&frontend_state_);
            for (auto& model : wake_word_models_) {
                if (!model || !model->ResetState()) {
                    ESP_LOGE(TAG, "Failed to reset wake word model; reloading");
                    UnloadModels();
                    break;
                }
            }
            if (models_loaded_ && (!vad_model_ || !vad_model_->ResetState())) {
                ESP_LOGE(TAG, "Failed to reset VAD model; reloading");
                UnloadModels();
            }
        }
    }

    if ((!models_loaded_ && !LoadModels()) || !AllocateBuffers()) {
        ESP_LOGE(TAG, "Failed to load models or allocate buffers");
        return;
    }

    if (!ResetStates()) return;
    running_ = true;
    ESP_LOGI(TAG, "MicroWakeWord started; VAD backend=esphome-neural, cutoff=%.3f, effective=%.6f, window=%u, score_step=%ums",
             micro_wake_word::kVadProbabilityCutoff, vad_model_->GetEffectiveProbabilityCutoff(),
             unsigned(micro_wake_word::kVadWindowSize),
             unsigned(vad_model_->GetInputStride() * features_step_size_));
}

void MicroWakeWord::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }

    running_ = false;
    ESP_LOGI(TAG, "MicroWakeWord paused");
}

bool MicroWakeWord::ResetInputState() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return true;
    }
    return ResetInputStateLocked();
}

bool MicroWakeWord::ResetInputStateLocked() {
    if (!frontend_initialized_ || !models_loaded_) {
        ESP_LOGE(TAG, "Cannot reset input state before frontend and models are ready");
        return false;
    }

    FrontendReset(&frontend_state_);
    for (auto& model : wake_word_models_) {
        if (!model || !model->ResetState()) {
            ESP_LOGE(TAG, "Failed to reset wake word model state");
            return false;
        }
    }
    if (!vad_model_ || !vad_model_->ResetState()) {
        ESP_LOGE(TAG, "Failed to reset VAD model state");
        return false;
    }
    if (!ResetStates()) return false;
    ESP_LOGI(TAG, "Wake word input state reset");
    return true;
}

void MicroWakeWord::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    bool was_running = running_;
    running_ = false;
    wake_score_capture_active_ = false;
    if (models_loaded_ || frontend_initialized_) {
        UnloadModels();
    }
    DeallocateBuffers();
    if (was_running) {
        ESP_LOGI(TAG, "MicroWakeWord stopped");
    }
}

size_t MicroWakeWord::GetFeedSize() {
    if (!running_) {
        return 0;
    }
    // Return the size needed for one processing step (in samples)
    // This matches the pattern used by other wake word implementations
    return GetNewSamplesToGet();
}

bool MicroWakeWord::LoadModels() {
    if (frontend_initialized_) {
        FrontendFreeStateContents(&frontend_state_);
        frontend_initialized_ = false;
    }
    memset(&frontend_state_, 0, sizeof(frontend_state_));
    
    if (!FrontendPopulateState(&frontend_config_, &frontend_state_, AUDIO_SAMPLE_FREQUENCY)) {
        ESP_LOGE(TAG, "Failed to re-populate frontend state during model loading");
        return false;
    }
    frontend_initialized_ = true;
    
    for (auto &model : wake_word_models_) {
        if (!model) {
            ESP_LOGE(TAG, "Null model pointer during loading");
            if (frontend_initialized_) {
                FrontendFreeStateContents(&frontend_state_);
                frontend_initialized_ = false;
            }
            return false;
        }
        
        if (!model->LoadModel(streaming_op_resolver_)) {
            ESP_LOGE(TAG, "Failed to load wake word model: %s", model->GetWakeWord().c_str());
            for (auto &loaded_model : wake_word_models_) {
                if (loaded_model.get() == model.get()) break;
                loaded_model->UnloadModel();
            }
            if (frontend_initialized_) {
                FrontendFreeStateContents(&frontend_state_);
                frontend_initialized_ = false;
            }
            return false;
        }
        model->LogModelConfig();
    }

    if (!vad_model_ || !vad_model_->LoadModel(streaming_op_resolver_)) {
        ESP_LOGE(TAG, "Failed to load neural VAD; detection disabled (no bypass)");
        UnloadModels();
        return false;
    }
    vad_model_->LogModelConfig();
    ESP_LOGI(TAG, "All models loaded successfully");
    models_loaded_ = true;
    return true;
}

void MicroWakeWord::UnloadModels() {
    if (vad_model_) vad_model_->UnloadModel();
    for (auto &model : wake_word_models_) {
        if (model) {
            model->UnloadModel();
        }
    }
    
    if (frontend_initialized_) {
        FrontendFreeStateContents(&frontend_state_);
        frontend_initialized_ = false;
    }
    
    models_loaded_ = false;
}

bool MicroWakeWord::AllocateBuffers() {
    if (preprocessor_audio_buffer_) {
        return true;
    }

    size_t buffer_size = GetNewSamplesToGet() * sizeof(int16_t);
    preprocessor_audio_buffer_ = (int16_t*)heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    if (!preprocessor_audio_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate preprocessor audio buffer");
        return false;
    }

    return true;
}

void MicroWakeWord::DeallocateBuffers() {
    if (preprocessor_audio_buffer_) {
        heap_caps_free(preprocessor_audio_buffer_);
        preprocessor_audio_buffer_ = nullptr;
    }
}

bool MicroWakeWord::UpdateModelProbabilities() {
    if (!models_loaded_) {
        ESP_LOGW(TAG, "Models not loaded, skipping inference");
        running_ = false;
        return false;
    }
    
    int8_t audio_features[PREPROCESSOR_FEATURE_SIZE];
    if (!GenerateFeaturesForWindow(audio_features)) {
        return false;
    }

    ignore_windows_ = std::min(ignore_windows_ + 1, 0);

    for (auto &model : wake_word_models_) {
        if (!model || !model->PerformStreamingInference(audio_features)) {
            ESP_LOGW(TAG, "Model inference failed: %s", 
                    model ? model->GetWakeWord().c_str() : "null");
            running_ = false;
            return false;
        }
        
#ifdef CONFIG_MICRO_WAKE_WORD_DEBUG
        static int inference_count = 0;
        if (++inference_count % 200 == 0) {
            ESP_LOGI(TAG, "Model '%s' inference #%d completed", 
                    model->GetWakeWord().c_str(), inference_count);
        }
#endif
    }
    if (!vad_model_ || !vad_model_->PerformStreamingInference(audio_features)) {
        ESP_LOGE(TAG, "Neural VAD inference failed; detection disabled (no bypass)");
        running_ = false;
        return false;
    }
    return true;
}

bool MicroWakeWord::DetectWakeWords() {
    // Verify we have processed enough samples since the last positive detection
    if (ignore_windows_ < 0) {
        static int ignore_count = 0;
        ignore_count++;
        if (ignore_count % 100 == 0) {
            ESP_LOGD(TAG, "Still in ignore period: ignore_windows_=%d (#%d)", ignore_windows_, ignore_count);
        }
        return false;
    }

    for (auto &model : wake_word_models_) {
        if (!model->HasNewProbability()) continue;
        if (model->DetermineDetected()) {
            const bool speech = vad_model_->DetermineDetected();
            if (speech || !vad_reject_logged_) {
                ESP_LOGW(TAG, "Wake VAD %s: '%s', backend=esphome-neural, wake_avg=%.4f, vad_avg=%.4f, cutoff=%.3f, effective=%.6f, window=%u, pcm_sample_ms=%llu",
                         speech ? "ACCEPT" : "REJECT", model->GetWakeWord().c_str(),
                         model->GetAverageProbability(), vad_model_->GetAverageProbability(),
                         micro_wake_word::kVadProbabilityCutoff, vad_model_->GetEffectiveProbabilityCutoff(),
                         unsigned(micro_wake_word::kVadWindowSize),
                         static_cast<unsigned long long>(consumed_samples_ / 16));
            }
            vad_reject_logged_ = !speech;
            if (!speech) {
                // ESPHome retains wake scores when VAD blocks a candidate.
                // Recheck on the next fresh score without a cooldown/reset.
                continue;
            }
            model->BeginWakeScoreCapture();
            detected_wake_word_ = model->GetWakeWord();
            ignore_windows_ = -MIN_SLICES_BEFORE_DETECTION; // Reset ignore counter
            return true;
        }
        vad_reject_logged_ = false;
    }
    return false;
}

bool MicroWakeWord::GenerateFeaturesForWindow(int8_t features[PREPROCESSOR_FEATURE_SIZE]) {
    if (!HasEnoughSamples()) {
        return false;
    }
    
    const size_t samples_needed = GetNewSamplesToGet();
    size_t samples_read = ReadFromRingBuffer(preprocessor_audio_buffer_, samples_needed);
    
    if (samples_read < samples_needed) {
        ESP_LOGD(TAG, "Partial read: got %zu samples, needed %zu", samples_read, samples_needed);
        return false;
    }

    // Tap only the PCM actually consumed by the model frontend, after wake-queue
    // drops/generation checks. SD I/O runs on a separate task.
    WakeAudioRecorder::GetInstance().Feed(preprocessor_audio_buffer_, samples_read);

    consumed_samples_ += samples_read;

    size_t num_samples_read;
    
    struct FrontendOutput frontend_output = FrontendProcessSamples(
        &frontend_state_, preprocessor_audio_buffer_, samples_needed, &num_samples_read);
    
    if (frontend_output.size != PREPROCESSOR_FEATURE_SIZE) {
        ESP_LOGD(TAG, "Frontend output size mismatch: expected %d, got %d", 
                PREPROCESSOR_FEATURE_SIZE, frontend_output.size);
        return false;
    }

    constexpr int32_t value_scale = 256;
    constexpr int32_t value_div = 666; // 25.6 * 26.0 rounded
    
    for (size_t i = 0; i < frontend_output.size; ++i) {
        int32_t value = ((frontend_output.values[i] * value_scale) + (value_div / 2)) / value_div - 128;
        features[i] = static_cast<int8_t>(std::clamp(value, static_cast<int32_t>(-128), static_cast<int32_t>(127)));
    }

    return true;
}

bool MicroWakeWord::ResetStates() {
    wake_score_capture_active_ = false;
    ring_buffer_read_pos_ = 0;
    ring_buffer_write_pos_ = 0;
    ignore_windows_ = -MIN_SLICES_BEFORE_DETECTION;
    
    for (auto &model : wake_word_models_) {
        model->ResetProbabilities();
    }
    if (!vad_model_ || vad_model_->GetInputStride() == 0) {
        running_ = false;
        ESP_LOGE(TAG, "Neural VAD unavailable; wake detection disabled (no bypass)");
        return false;
    }
    vad_model_->ResetProbabilities();
    vad_reject_logged_ = false;
    consumed_samples_ = 0;
    return true;
}

bool MicroWakeWord::RegisterStreamingOps(tflite::MicroMutableOpResolver<20> &op_resolver) {
    // Register TensorFlow Lite operations needed for the streaming models
    if (op_resolver.AddCallOnce() != kTfLiteOk) return false;
    if (op_resolver.AddVarHandle() != kTfLiteOk) return false;
    if (op_resolver.AddReshape() != kTfLiteOk) return false;
    if (op_resolver.AddReadVariable() != kTfLiteOk) return false;
    if (op_resolver.AddStridedSlice() != kTfLiteOk) return false;
    if (op_resolver.AddConcatenation() != kTfLiteOk) return false;
    if (op_resolver.AddAssignVariable() != kTfLiteOk) return false;
    if (op_resolver.AddConv2D() != kTfLiteOk) return false;
    if (op_resolver.AddMul() != kTfLiteOk) return false;
    if (op_resolver.AddAdd() != kTfLiteOk) return false;
    if (op_resolver.AddMean() != kTfLiteOk) return false;
    if (op_resolver.AddFullyConnected() != kTfLiteOk) return false;
    if (op_resolver.AddLogistic() != kTfLiteOk) return false;
    if (op_resolver.AddQuantize() != kTfLiteOk) return false;
    if (op_resolver.AddDepthwiseConv2D() != kTfLiteOk) return false;
    if (op_resolver.AddAveragePool2D() != kTfLiteOk) return false;
    if (op_resolver.AddMaxPool2D() != kTfLiteOk) return false;
    if (op_resolver.AddPad() != kTfLiteOk) return false;
    if (op_resolver.AddPack() != kTfLiteOk) return false;
    if (op_resolver.AddSplitV() != kTfLiteOk) return false;

    return true;
}

bool MicroWakeWord::HasEnoughSamples() {
    return GetRingBufferAvailable() >= GetNewSamplesToGet();
}

size_t MicroWakeWord::WriteToRingBuffer(const int16_t* data, size_t samples) {
    if (!data || samples == 0) {
        return 0;
    }

    size_t samples_written = 0;
    
    for (size_t i = 0; i < samples; ++i) {
        ring_buffer_[ring_buffer_write_pos_] = data[i];
        ring_buffer_write_pos_ = (ring_buffer_write_pos_ + 1) % ring_buffer_size_;
        
        // If we're about to overwrite unread data, advance read position
        if (ring_buffer_write_pos_ == ring_buffer_read_pos_) {
            ring_buffer_read_pos_ = (ring_buffer_read_pos_ + 1) % ring_buffer_size_;
        }
        
        samples_written++;
    }

    return samples_written;
}

size_t MicroWakeWord::ReadFromRingBuffer(int16_t* data, size_t samples) {
    if (!data || samples == 0) {
        return 0;
    }

    size_t available = GetRingBufferAvailable();
    size_t samples_to_read = std::min(samples, available);
    
    for (size_t i = 0; i < samples_to_read; ++i) {
        data[i] = ring_buffer_[ring_buffer_read_pos_];
        ring_buffer_read_pos_ = (ring_buffer_read_pos_ + 1) % ring_buffer_size_;
    }

    return samples_to_read;
}

size_t MicroWakeWord::GetRingBufferAvailable() {
    if (ring_buffer_write_pos_ >= ring_buffer_read_pos_) {
        return ring_buffer_write_pos_ - ring_buffer_read_pos_;
    } else {
        return ring_buffer_size_ - ring_buffer_read_pos_ + ring_buffer_write_pos_;
    }
}
