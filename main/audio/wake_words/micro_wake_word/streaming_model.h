#pragma once

#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>

#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

// Preprocessor settings
#ifndef PREPROCESSOR_FEATURE_SIZE
#define PREPROCESSOR_FEATURE_SIZE 40
#endif

namespace micro_wake_word {

    static const uint32_t STREAMING_MODEL_VARIABLE_ARENA_SIZE = 1024;

    class StreamingModel {
    public:
        virtual ~StreamingModel() = default;
        virtual void LogModelConfig() = 0;
        virtual bool DetermineDetected() = 0;

        bool PerformStreamingInference(const int8_t features[PREPROCESSOR_FEATURE_SIZE]);

        /// @brief Sets all recent_streaming_probabilities to 0
        void ResetProbabilities();

        /// @brief Allocates tensor and variable arenas and sets up the model interpreter
        /// @param op_resolver MicroMutableOpResolver object that must exist until the model is unloaded
        /// @return True if successful, false otherwise
        bool LoadModel(tflite::MicroMutableOpResolver<20> &op_resolver);

        /// @brief Destroys the TFLite interpreter and frees the tensor and variable arenas' memory
        void UnloadModel();

    protected:
        uint8_t current_stride_step_{0};

        float probability_cutoff_;
        size_t sliding_window_size_;
        size_t last_n_index_{0};
        size_t tensor_arena_size_;
        std::vector<float> recent_streaming_probabilities_;

        const uint8_t *model_start_;
        uint8_t *tensor_arena_{nullptr};
        std::unique_ptr<tflite::MicroInterpreter> interpreter_;
        tflite::MicroResourceVariables *mrv_{nullptr};
        tflite::MicroAllocator *ma_{nullptr};
        
        mutable std::mutex inference_mutex_;
        mutable std::atomic<bool> model_loaded_{false};
    };

    class WakeWordModel final : public StreamingModel {
    public:
        WakeWordModel(const uint8_t *model_start, float probability_cutoff, 
                      size_t sliding_window_average_size, const std::string &wake_word, 
                      size_t tensor_arena_size);
        
        ~WakeWordModel();

        void LogModelConfig() override;

        /// @brief Checks for the wake word by comparing the mean probability in the sliding window
        /// @return True if wake word detected, false otherwise
        bool DetermineDetected() override;

        const std::string &GetWakeWord() const {
            return wake_word_;
        }

    protected:
        std::string wake_word_;
    };

    class VADModel final : public StreamingModel {
    public:
        VADModel(const uint8_t *model_start, float probability_cutoff, 
                 size_t sliding_window_size, size_t tensor_arena_size);
        
        ~VADModel();

        void LogModelConfig() override;

        /// @brief Checks for voice activity by comparing the max probability in the sliding window
        /// @return True if voice activity detected, false otherwise
        bool DetermineDetected() override;
    };

} // namespace micro_wake_word
