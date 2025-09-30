#ifndef __MICRO_WAKE_WORD_H__
#define __MICRO_WAKE_WORD_H__

#include "wake_word.h"
#include "audio_codec.h"
#include "micro_wake_word/streaming_model.h"
#include "micro_wake_word/preprocessor_settings.h"

#include <vector>
#include <string>
#include <functional>
#include <memory>

// TensorFlow Lite includes
#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

// Audio frontend includes
#include <frontend_util.h>

class MicroWakeWord : public WakeWord {
public:
    MicroWakeWord(const void* model_data, size_t model_size);
    ~MicroWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override;
    void Feed(const std::vector<int16_t>& data) override;
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }

private:
    const void* model_data_;
    size_t model_size_;

    // Core members
    AudioCodec* codec_ = nullptr;
    std::string last_detected_wake_word_;
    std::function<void(const std::string&)> detection_callback_;
    
    // State management
    bool initialized_ = false;
    bool running_ = false;
    bool detected_ = false;
    bool frontend_initialized_ = false;
    bool models_loaded_ = false;  // 添加模型加载状态标志
    std::string detected_wake_word_;

    // Audio processing
    uint8_t features_step_size_ = 10;
    std::vector<int16_t> audio_buffer_;
    std::vector<int16_t> ring_buffer_;
    size_t ring_buffer_read_pos_ = 0;
    size_t ring_buffer_write_pos_ = 0;
    size_t ring_buffer_size_ = 0;
    
    // Model management
    std::vector<std::unique_ptr<micro_wake_word::WakeWordModel>> wake_word_models_;
    tflite::MicroMutableOpResolver<20> streaming_op_resolver_;
    
    // Audio frontend
    struct FrontendConfig frontend_config_;
    struct FrontendState frontend_state_;
    int16_t ignore_windows_ = -74; // MIN_SLICES_BEFORE_DETECTION
    
    // Buffers
    int16_t *preprocessor_audio_buffer_ = nullptr;
    
    // Private methods
    bool LoadModels();
    void UnloadModels();
    bool AllocateBuffers();
    void DeallocateBuffers();
    void UpdateModelProbabilities();
    bool DetectWakeWords();
    bool GenerateFeaturesForWindow(int8_t features[PREPROCESSOR_FEATURE_SIZE]);
    void ResetStates();
    bool RegisterStreamingOps(tflite::MicroMutableOpResolver<20> &op_resolver);
    bool HasEnoughSamples();
    uint16_t GetNewSamplesToGet() { return features_step_size_ * 16; } // 16kHz / 1000ms * step_size
    size_t WriteToRingBuffer(const int16_t* data, size_t samples);
    size_t ReadFromRingBuffer(int16_t* data, size_t samples);
    size_t GetRingBufferAvailable();
};

#endif // __MICRO_WAKE_WORD_H__
