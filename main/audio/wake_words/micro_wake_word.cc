#include "micro_wake_word.h"
#include "micro_wake_word/streaming_model.h"
#include "micro_wake_word/preprocessor_settings.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_spiffs.h>
#include <spi_flash_mmap.h>
#include <frontend.h>
#include <frontend_util.h>
#include <cstring>
#include <algorithm>

#define TAG "MicroWakeWord"

extern const uint8_t model_start[] asm("_binary_" MICRO_WAKE_WORD_MODEL_NAME "_tflite_start");

MicroWakeWord::MicroWakeWord() {
    // Initialize ring buffer
    ring_buffer_size_ = BUFFER_SIZE;
    ring_buffer_.resize(ring_buffer_size_);
    
    // Initialize frontend config - will be set properly in Initialize()
    memset(&frontend_config_, 0, sizeof(frontend_config_));
    memset(&frontend_state_, 0, sizeof(frontend_state_));
}

MicroWakeWord::~MicroWakeWord() {
    Stop();
    DeallocateBuffers();
    UnloadModels();
}

bool MicroWakeWord::Initialize(AudioCodec* codec) {
    if (initialized_) {
        return true;
    }

    codec_ = codec;
    
    if (!RegisterStreamingOps(streaming_op_resolver_)) {
        ESP_LOGE(TAG, "Failed to register streaming operations");
        return false;
    }

    // Configure audio frontend
    frontend_config_.window.size_ms = FEATURE_DURATION_MS;
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

    ESP_LOGI(TAG, "Loading wake word model %s from %p", MICRO_WAKE_WORD_MODEL_NAME, model_start);

    auto model = std::make_unique<micro_wake_word::WakeWordModel>(
        model_start,
        (float)MICRO_WAKE_WORD_MODEL_PROBABILITY_CUTOFF / 100.0f,
        5,
        MICRO_WAKE_WORD_MODEL_WAKE_WORD,
        128 * 1024);

    wake_word_models_.push_back(std::move(model));

    initialized_ = true;
    ESP_LOGI(TAG, "MicroWakeWord initialized successfully");
    return true;
}

void MicroWakeWord::Feed(const std::vector<int16_t>& data) {
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

    // Write to ring buffer
    WriteToRingBuffer(data.data(), data.size());

    // Process available data
    while (HasEnoughSamples()) {
        UpdateModelProbabilities();
        
        if (DetectWakeWords()) {
            ESP_LOGI(TAG, "Wake Word '%s' Detected", detected_wake_word_.c_str());
            last_detected_wake_word_ = detected_wake_word_;
            detected_ = true;
            
            if (detection_callback_) {
                detection_callback_(detected_wake_word_);
            }
            
            // Reset for next detection
            detected_wake_word_.clear();
            detected_ = false;
            ResetStates();
        }
    }
}

void MicroWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    detection_callback_ = callback;
}

void MicroWakeWord::Start() {
    if (!initialized_) {
        ESP_LOGE(TAG, "MicroWakeWord not initialized");
        return;
    }

    if (running_) {
        ESP_LOGW(TAG, "MicroWakeWord already running");
        return;
    }

    if (!LoadModels() || !AllocateBuffers()) {
        ESP_LOGE(TAG, "Failed to load models or allocate buffers");
        return;
    }

    ResetStates();
    running_ = true;
    ESP_LOGI(TAG, "MicroWakeWord started");
}

void MicroWakeWord::Stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    UnloadModels();
    DeallocateBuffers();
    ESP_LOGI(TAG, "MicroWakeWord stopped");
}

size_t MicroWakeWord::GetFeedSize() {
    if (!running_) {
        return 0;
    }
    // Return the size needed for one processing step (in samples)
    // This matches the pattern used by other wake word implementations
    return GetNewSamplesToGet();
}

void MicroWakeWord::EncodeWakeWordData() {
    // Not implemented for this version
}

bool MicroWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    // Not implemented for this version
    return false;
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

    ESP_LOGI(TAG, "All models loaded successfully");
    models_loaded_ = true;
    return true;
}

void MicroWakeWord::UnloadModels() {
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

void MicroWakeWord::UpdateModelProbabilities() {
    if (!models_loaded_) {
        ESP_LOGW(TAG, "Models not loaded, skipping inference");
        return;
    }
    
    int8_t audio_features[PREPROCESSOR_FEATURE_SIZE];
    if (!GenerateFeaturesForWindow(audio_features)) {
        return;
    }

    ignore_windows_ = std::min(ignore_windows_ + 1, 0);

    for (auto &model : wake_word_models_) {
        if (!model || !model->PerformStreamingInference(audio_features)) {
            ESP_LOGW(TAG, "Model inference failed: %s", 
                    model ? model->GetWakeWord().c_str() : "null");
            continue;
        }
        
#ifdef CONFIG_MICRO_WAKE_WORD_DEBUG
        static int inference_count = 0;
        if (++inference_count % 200 == 0) {
            ESP_LOGI(TAG, "Model '%s' inference #%d completed", 
                    model->GetWakeWord().c_str(), inference_count);
        }
#endif
    }
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
        if (model->DetermineDetected()) {
            detected_wake_word_ = model->GetWakeWord();
            ignore_windows_ = -MIN_SLICES_BEFORE_DETECTION; // Reset ignore counter
            ESP_LOGI(TAG, "WAKE WORD DETECTED: '%s'", detected_wake_word_.c_str());
            return true;
        }
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

void MicroWakeWord::ResetStates() {
    ring_buffer_read_pos_ = 0;
    ring_buffer_write_pos_ = 0;
    ignore_windows_ = -MIN_SLICES_BEFORE_DETECTION;
    
    for (auto &model : wake_word_models_) {
        model->ResetProbabilities();
    }
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

