#include "afe_wake_word.h"

#include <esp_log.h>
#include <sstream>

#define DETECTION_RUNNING_EVENT 1

#define TAG "AfeWakeWord"

AfeWakeWord::AfeWakeWord() : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }

    vEventGroupDelete(event_group_);
}

bool AfeWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    int ref_num = codec_->input_reference() ? 1 : 0;

    if (models_list == nullptr) {
        models_ = esp_srmodel_init("model");
    } else {
        models_ = models_list;
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }
    for (int i = 0; i < models_->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        if (strstr(models_->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model_ = models_->model_name[i];
            auto words = esp_srmodel_get_wake_words(models_, wakenet_model_);
            // split by ";" to get all wake words
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                wake_words_.push_back(word);
            }
        }
    }

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models_, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 8;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    
    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);

    // xTaskCreate([](void* arg) {
    //     auto this_ = (AfeWakeWord*)arg;
    //     this_->AudioDetectionTask();
    //     vTaskDelete(NULL);
    // }, "audio_detection", 4096, this, 8, nullptr);

    return true;
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void AfeWakeWord::Start() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

size_t AfeWakeWord::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::AudioDetectionTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            Stop();
            last_detected_wake_word_ = wake_words_[res->wakenet_model_index - 1];

            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
    }
}
