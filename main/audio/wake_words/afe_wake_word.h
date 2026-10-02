#ifndef AFE_WAKE_WORD_H
#define AFE_WAKE_WORD_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <esp_afe_sr_models.h>
#include <esp_nsn_models.h>
#include <model_path.h>

#include <string>
#include <vector>
#include <functional>

#include "codecs/audio_codec.h"
#include "wake_word.h"

class AfeWakeWord : public WakeWord {
public:
    AfeWakeWord();
    ~AfeWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list);
    void Feed(const std::vector<int16_t>& data);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback);
    void Start();
    void Stop();
    size_t GetFeedSize();
    const std::string& GetLastDetectedWakeWord() const { return last_detected_wake_word_; }

private:
    srmodel_list_t *models_ = nullptr;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    char* wakenet_model_ = NULL;
    std::vector<std::string> wake_words_;
    EventGroupHandle_t event_group_;
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    AudioCodec* codec_ = nullptr;
    std::string last_detected_wake_word_;

    void AudioDetectionTask();
};

#endif
