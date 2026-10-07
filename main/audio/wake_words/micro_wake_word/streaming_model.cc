#include "streaming_model.h"
#include "preprocessor_settings.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
// #include <esp_cache.h>
#include <cstring>
#include <algorithm>
#include <numeric>

static const char *TAG = "StreamingModel";

namespace micro_wake_word {

bool StreamingModel::PerformStreamingInference(const int8_t features[PREPROCESSOR_FEATURE_SIZE]) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    
#ifdef CONFIG_MICRO_WAKE_WORD_DEBUG
    // 调试：检查输入特征
    static int feature_count = 0;
    feature_count++;
    if (feature_count % 500 == 0) {
        bool all_zero = true;
        int8_t min_val = 127, max_val = -128;
        for (int i = 0; i < PREPROCESSOR_FEATURE_SIZE; i++) {
            if (features[i] != 0) all_zero = false;
            min_val = std::min(min_val, features[i]);
            max_val = std::max(max_val, features[i]);
        }
        ESP_LOGI(TAG, "Input features #%d: all_zero=%s, min=%d, max=%d, first_few=[%d,%d,%d,%d]", 
                feature_count, all_zero ? "yes" : "no", min_val, max_val,
                features[0], features[1], features[2], features[3]);
    }
#endif
    // 首先检查原子状态标志
    if (!model_loaded_.load()) {
        ESP_LOGE(TAG, "Model not loaded (atomic flag check)");
        return false;
    }
    
    if (!interpreter_) {
        ESP_LOGE(TAG, "Interpreter is null, model not loaded or corrupted");
        model_loaded_.store(false);  // 标记模型为未加载状态
        return false;
    }

    // 额外的安全检查
    if (!tensor_arena_ || !variable_arena_ || !variable_allocator_ || !mrv_) {
        ESP_LOGE(TAG,
                 "Model components not properly initialized "
                 "(tensor_arena:%p, variable_arena:%p, allocator:%p, mrv:%p)",
                 tensor_arena_, variable_arena_, variable_allocator_, mrv_);
        model_loaded_.store(false);  // 标记模型为未加载状态
        return false;
    }

    TfLiteTensor *input = interpreter_->input(0);
    if (!input) {
        ESP_LOGE(TAG, "Failed to get input tensor");
        return false;
    }
#ifdef CONFIG_MICRO_WAKE_WORD_DEBUG
    // 调试：检查输入张量信息
    static bool logged_input_info = false;
    if (!logged_input_info) {
        ESP_LOGI(TAG, "=== INPUT TENSOR DEBUG ===");
        ESP_LOGI(TAG, "Input tensor info:");
        ESP_LOGI(TAG, "  Type: %d", input->type);
        ESP_LOGI(TAG, "  Dims: %d", input->dims->size);
        for (int i = 0; i < input->dims->size; i++) {
            ESP_LOGI(TAG, "  Dim[%d]: %d", i, input->dims->data[i]);
        }
        ESP_LOGI(TAG, "  Bytes: %d", input->bytes);
        ESP_LOGI(TAG, "  Expected feature size: %d", PREPROCESSOR_FEATURE_SIZE);
        logged_input_info = true;
    }
#endif
    // Copy input data - like ESPHome implementation
    // Copy features to the current stride position
    std::memcpy(input->data.int8 + PREPROCESSOR_FEATURE_SIZE * current_stride_step_,
                features, PREPROCESSOR_FEATURE_SIZE * sizeof(int8_t));
    
    current_stride_step_++;

    // Get stride from input tensor dimensions and check if we need more steps
    const uint8_t stride = input->dims->data[1];  // Time steps dimension
    
    ESP_LOGV(TAG, "Stride step %d/%d", current_stride_step_, stride);
    
    // Only run inference when we have collected enough stride steps
    if (current_stride_step_ < stride) {
        ESP_LOGV(TAG, "Waiting for more stride steps (%d/%d)", current_stride_step_, stride);
        return true;  // Don't run inference yet, but return success
    }
    
    // Reset stride step for next sequence
    current_stride_step_ = 0;
    
    ESP_LOGV(TAG, "Running inference with full stride sequence");

    // Run inference
    TfLiteStatus invoke_status = interpreter_->Invoke();
    if (invoke_status != kTfLiteOk) {
        ESP_LOGE(TAG, "Inference failed");
        return false;
    }

    // 再次检查interpreter_是否仍然有效（可能在Invoke过程中被损坏）
    if (!interpreter_) {
        ESP_LOGE(TAG, "Interpreter became null after Invoke()");
        return false;
    }

    // Get output probability
    TfLiteTensor *output = interpreter_->output(0);
    if (!output) {
        ESP_LOGE(TAG, "Failed to get output tensor");
        return false;
    }
    
    // 检查输出张量数据是否有效
    if (!output->data.raw) {
        ESP_LOGE(TAG, "Output tensor data is null");
        return false;
    }
    
#ifdef CONFIG_MICRO_WAKE_WORD_DEBUG
    // 调试：检查输出张量信息
    static bool logged_output_info = false;
    if (!logged_output_info) {
        ESP_LOGI(TAG, "Output tensor info:");
        ESP_LOGI(TAG, "  Type: %d", output->type);
        ESP_LOGI(TAG, "  Dims: %d", output->dims->size);
        for (int i = 0; i < output->dims->size; i++) {
            ESP_LOGI(TAG, "  Dim[%d]: %d", i, output->dims->data[i]);
        }
        ESP_LOGI(TAG, "  Bytes: %d", output->bytes);
        logged_output_info = true;
    }
#endif

    float probability = 0.0f;
    
    // 根据张量类型读取概率值
    if (output->type == kTfLiteFloat32) {
        // Float模型：直接使用输出值
        probability = output->data.f[0];
    } else if (output->type == kTfLiteInt8) {
        // Int8量化模型：转换为float范围 [0, 1]
        int8_t quantized_value = output->data.int8[0];
        probability = (quantized_value + 128) / 255.0f;
        ESP_LOGD(TAG, "Int8 output: %d -> %.6f", quantized_value, probability);
    } else if (output->type == kTfLiteUInt8) {
        // UInt8量化模型：使用正确的反量化公式
        uint8_t raw_uint8 = output->data.uint8[0];
        
        if (output->params.scale != 0.0f) {
            // 使用模型的量化参数进行反量化
            probability = (raw_uint8 - output->params.zero_point) * output->params.scale;
        } else {
            // 使用默认量化参数 (从ESPHome验证得出)
            probability = raw_uint8 * 0.003906f;  // scale=0.003906, zero_point=0
        }
        ESP_LOGD(TAG, "UInt8 output: %d -> %.6f", raw_uint8, probability);
    } else {
        ESP_LOGE(TAG, "Unsupported output tensor type: %d", output->type);
        return false;
    }

#if CONFIG_MICRO_WAKE_WORD_DEBUG
    // 调试：只在概率较高时显示
    static int prob_count = 0;
    prob_count++;
    if (probability > 0.05f || prob_count % 2000 == 0) {
        ESP_LOGI(TAG, "Model probability=%.6f, cutoff=%.4f (#%d)", 
                probability, probability_cutoff_, prob_count);
    }
#endif

    recent_streaming_probabilities_[last_n_index_] = probability;
    last_n_index_ = (last_n_index_ + 1) % sliding_window_size_;

    return true;
}

void StreamingModel::ResetProbabilities() {
    std::fill(recent_streaming_probabilities_.begin(), recent_streaming_probabilities_.end(), 0);
    last_n_index_ = 0;
}

bool StreamingModel::ResetState() {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    if (!model_loaded_.load() || !interpreter_ || interpreter_->Reset() != kTfLiteOk) {
        return false;
    }
    current_stride_step_ = 0;
    ResetProbabilities();
    return true;
}

bool StreamingModel::LoadModel(tflite::MicroMutableOpResolver<20> &op_resolver) {
    std::lock_guard<std::mutex> lock(inference_mutex_);

    ESP_LOGI(TAG, "Loading model with tensor arena size: %" PRIu32, (uint32_t)tensor_arena_size_);

    // MicroWakeWord 只在带 PSRAM 的目标上可用。Tensor arena 会长期占用
    // 数万字节，若放在内部 RAM 会挤占 Wi-Fi 管理帧和任务栈空间。
    // 因此强制分配到 PSRAM；分配失败时不回退到内部 RAM，保证系统可用。
    tensor_arena_ = (uint8_t *)heap_caps_aligned_alloc(
        64, tensor_arena_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tensor_arena_) {
        ESP_LOGE(TAG,
                 "Failed to allocate tensor arena in SPIRAM: requested=%u, "
                 "free_spiram=%u, free_internal=%u",
                 (unsigned int)tensor_arena_size_,
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    ESP_LOGI(TAG,
             "Allocated tensor arena in SPIRAM with 64-byte alignment: %p "
             "(free_internal=%u)",
             tensor_arena_,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Set up the model
    const tflite::Model *model = tflite::GetModel(model_start_);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema version %" PRIu32 " doesn't match supported version %d", 
                model->version(), TFLITE_SCHEMA_VERSION);
        UnloadModelLocked();
        return false;
    }

    // Resource variables use their own small arena. The second argument of
    // MicroResourceVariables::Create is a variable count, not an arena size.
    variable_arena_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        16, STREAMING_MODEL_VARIABLE_ARENA_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!variable_arena_) {
        ESP_LOGE(TAG,
                 "Failed to allocate resource-variable arena in SPIRAM: "
                 "requested=%u",
                 static_cast<unsigned>(STREAMING_MODEL_VARIABLE_ARENA_SIZE));
        UnloadModelLocked();
        return false;
    }

    variable_allocator_ = tflite::MicroAllocator::Create(
        variable_arena_, STREAMING_MODEL_VARIABLE_ARENA_SIZE);
    if (!variable_allocator_) {
        ESP_LOGE(TAG, "Failed to create resource-variable allocator");
        UnloadModelLocked();
        return false;
    }

    mrv_ = tflite::MicroResourceVariables::Create(
        variable_allocator_, STREAMING_MODEL_MAX_RESOURCE_VARIABLES);
    if (!mrv_) {
        ESP_LOGE(TAG, "Failed to create MicroResourceVariables");
        UnloadModelLocked();
        return false;
    }

    // Create interpreter
    interpreter_ = std::make_unique<tflite::MicroInterpreter>(
        model, op_resolver, tensor_arena_, tensor_arena_size_, mrv_);
    if (!interpreter_) {
        ESP_LOGE(TAG, "Failed to create MicroInterpreter");
        UnloadModelLocked();
        return false;
    }

    // Allocate tensors
    TfLiteStatus allocate_status = interpreter_->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors() failed");
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGE(TAG, "Available SPIRAM: %u bytes, Internal RAM: %u bytes", (unsigned int)free_heap, (unsigned int)free_internal);

        // 检查tensor arena是否足够
        ESP_LOGE(TAG, "Tensor arena size: %u bytes", (unsigned int)tensor_arena_size_);
        UnloadModelLocked();
        return false;
    }

    const size_t used_bytes = interpreter_->arena_used_bytes();
    ESP_LOGI(TAG,
             "Actual tensor arena size used: %u bytes (allocated: %u bytes)",
             static_cast<unsigned>(used_bytes),
             static_cast<unsigned>(tensor_arena_size_));
    ESP_LOGI(TAG,
             "Resource-variable arena used: %u bytes (allocated: %u bytes, "
             "max_variables=%d)",
             static_cast<unsigned>(variable_allocator_->used_bytes()),
             static_cast<unsigned>(STREAMING_MODEL_VARIABLE_ARENA_SIZE),
             STREAMING_MODEL_MAX_RESOURCE_VARIABLES);

    // 初始化资源变量以避免 "non-existent resource variable" 错误
    if (mrv_) {
        ESP_LOGI(TAG, "Initializing TensorFlow Lite resource variables");
    }

    // Initialize sliding window
    recent_streaming_probabilities_.resize(sliding_window_size_, 0);
    ResetProbabilities();

    // 标记模型为已加载状态
    model_loaded_.store(true);

    return true;
}

void StreamingModel::UnloadModel() {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    UnloadModelLocked();
}

void StreamingModel::UnloadModelLocked() {
    ESP_LOGI(TAG, "Starting model unload process...");
    
    // 立即标记模型为未加载状态，防止并发访问
    model_loaded_.store(false);
    
    if (interpreter_) {
        try {
            ESP_LOGI(TAG, "Resetting interpreter...");
            interpreter_.reset();  // 这会将指针设为nullptr
            interpreter_ = nullptr;  // 显式设为nullptr确保安全
            ESP_LOGI(TAG, "Interpreter reset successfully");
        } catch (...) {
            ESP_LOGW(TAG, "Exception during interpreter cleanup");
            interpreter_ = nullptr;  // 即使异常也要设为nullptr
        }
    }
    
    // 清理MicroResourceVariables
    if (mrv_) {
        ESP_LOGI(TAG, "Cleaning MicroResourceVariables...");
        mrv_ = nullptr;
    }
    
    variable_allocator_ = nullptr;

    if (variable_arena_) {
        ESP_LOGI(TAG, "Freeing resource-variable arena...");
        heap_caps_free(variable_arena_);
        variable_arena_ = nullptr;
        ESP_LOGI(TAG, "Resource-variable arena freed");
    }
    
    // 最后释放tensor arena
    if (tensor_arena_) {
        ESP_LOGI(TAG, "Freeing tensor arena...");
        heap_caps_free(tensor_arena_);
        tensor_arena_ = nullptr;
        ESP_LOGI(TAG, "Tensor arena freed");
    }
    
    ESP_LOGI(TAG, "Model unloaded successfully");
}

// WakeWordModel implementation
WakeWordModel::WakeWordModel(const uint8_t *model_start, float probability_cutoff,
                           size_t sliding_window_average_size, const std::string &wake_word,
                           size_t tensor_arena_size) : wake_word_(wake_word) {
    model_start_ = model_start;
    probability_cutoff_ = probability_cutoff;
    sliding_window_size_ = sliding_window_average_size;
    tensor_arena_size_ = tensor_arena_size;
}

WakeWordModel::~WakeWordModel() {
    UnloadModel();
}

void WakeWordModel::LogModelConfig() {
    ESP_LOGI(TAG, "Wake Word: %s", wake_word_.c_str());
    ESP_LOGI(TAG, "  Probability cutoff: %.4f", probability_cutoff_);
    ESP_LOGI(TAG, "  Sliding window size: %d", sliding_window_size_);
    ESP_LOGI(TAG, "  Tensor arena size: %d bytes", tensor_arena_size_);
}

bool WakeWordModel::DetermineDetected() {
    if (recent_streaming_probabilities_.empty()) {
        return false;
    }

    float sum = 0.0f;
    for (auto &prob : recent_streaming_probabilities_) {
        sum += prob;
    }
    
    float sliding_window_average = sum / static_cast<float>(sliding_window_size_);

#ifdef CONFIG_MICRO_WAKE_WORD_DEBUG
    // 调试：监控检测阈值
    static int detect_count = 0;
    detect_count++;
    if (detect_count % 1000 == 0 || sliding_window_average > 0.1f) {
        ESP_LOGI(TAG, "Model '%s' detection check #%d: sliding_avg=%.4f, cutoff=%.4f, window_size=%d", 
                wake_word_.c_str(), detect_count, sliding_window_average, probability_cutoff_, sliding_window_size_);
        
        size_t n = std::min(recent_streaming_probabilities_.size(), size_t(5));
        ESP_LOGI(TAG, "Recent float probs: [%.3f,%.3f,%.3f,%.3f,%.3f]", 
                n > 0 ? recent_streaming_probabilities_[0] : 0.0f,
                n > 1 ? recent_streaming_probabilities_[1] : 0.0f,
                n > 2 ? recent_streaming_probabilities_[2] : 0.0f,
                n > 3 ? recent_streaming_probabilities_[3] : 0.0f,
                n > 4 ? recent_streaming_probabilities_[4] : 0.0f);
    }
#endif

    // 检测唤醒词 - 如果滑动窗口平均值超过阈值
    if (sliding_window_average > probability_cutoff_) {
        ESP_LOGW(TAG,
                 "The '%s' model sliding average probability is %.3f and most recent "
                 "probability is %.3f",
                 wake_word_.c_str(), sliding_window_average,
                 recent_streaming_probabilities_[(last_n_index_ + sliding_window_size_ - 1) % sliding_window_size_]);
        return true;
    }

    return false;
}

// VADModel implementation
VADModel::VADModel(const uint8_t *model_start, float probability_cutoff,
                   size_t sliding_window_size, size_t tensor_arena_size) {
    model_start_ = model_start;
    probability_cutoff_ = probability_cutoff;
    sliding_window_size_ = sliding_window_size;
    tensor_arena_size_ = tensor_arena_size;
}

VADModel::~VADModel() {
    UnloadModel();
}

void VADModel::LogModelConfig() {
    ESP_LOGI(TAG, "VAD Model:");
    ESP_LOGI(TAG, "  Probability cutoff: %.4f", probability_cutoff_);
    ESP_LOGI(TAG, "  Sliding window size: %d", sliding_window_size_);
    ESP_LOGI(TAG, "  Tensor arena size: %d bytes", tensor_arena_size_);
}

bool VADModel::DetermineDetected() {
    if (recent_streaming_probabilities_.empty()) {
        return false;
    }

    // Find max probability
    auto max_it = std::max_element(recent_streaming_probabilities_.begin(), 
                                  recent_streaming_probabilities_.end());
    float max_prob = *max_it;

    return max_prob >= probability_cutoff_;
}

} // namespace micro_wake_word
