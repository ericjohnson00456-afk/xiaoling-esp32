#ifndef AFE_AUDIO_PROCESSOR_H
#define AFE_AUDIO_PROCESSOR_H

#include <esp_afe_sr_models.h>
#include <esp_ns.h>
#include <esp_agc.h>
#include <esp_vad.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <string>
#include <vector>
#include <functional>

#include "audio_processor.h"
#include "codecs/audio_codec.h"

class AfeAudioProcessor : public AudioProcessor {
public:
    // AudioService 内部任务统一使用优先级 8；播放器解码任务属于 AudioPlayer。
    static constexpr UBaseType_t kTaskPriority = 8;
    static constexpr BaseType_t kTaskCore = 1;

    AfeAudioProcessor();
    ~AfeAudioProcessor();

    void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void SetAecEnabled(bool enable) override;

private:
    EventGroupHandle_t event_group_ = nullptr;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
    bool is_speaking_ = false;
    std::vector<int16_t> output_buffer_;
    // ESP-SR 2.2 的 VC AFE 不安全地接入 WebRTC NS；在 AEC 输出后独立处理。
    ns_handle_t output_ns_ = nullptr;
    // 与会话 VAD 分离，在 NS 后判断人声，避免 AEC 模式关闭 VAD 时丢失判断。
    vad_handle_t gain_vad_ = nullptr;
    void* output_agc_ = nullptr;
    float agc_mix_ = 0.0f;
    uint32_t agc_frames_ = 0;

    void AudioProcessorTask();
    void EmitOutput(std::vector<int16_t>&& data);
    void ApplySpeechAgc(std::vector<int16_t>& data);
};

#endif
