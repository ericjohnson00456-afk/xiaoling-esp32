#include "custom_wake_word.h"
#include "system/system_info.h"
#include "resource/resource.h"

#include <esp_log.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <cJSON.h>


#define TAG "CustomWakeWord"


CustomWakeWord::CustomWakeWord() = default;

CustomWakeWord::~CustomWakeWord() {
    if (multinet_model_data_ != nullptr && multinet_ != nullptr) {
        multinet_->destroy(multinet_model_data_);
        multinet_model_data_ = nullptr;
    }

    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }
}

void CustomWakeWord::ParseWakenetModelConfig() {
    // Read index.json
    ResourceData asset;
    if (!Resource::Get(ResourcePackage::Respak, "index.json", asset)) {
        ESP_LOGE(TAG, "Failed to read index.json");
        return;
    }
    cJSON* root = cJSON_ParseWithLength(
        reinterpret_cast<const char*>(asset.address), asset.size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse index.json");
        return;
    }
    cJSON* multinet_model = cJSON_GetObjectItem(root, "multinet_model");
    if (cJSON_IsObject(multinet_model)) {
        cJSON* language = cJSON_GetObjectItem(multinet_model, "language");
        cJSON* duration = cJSON_GetObjectItem(multinet_model, "duration");
        cJSON* threshold = cJSON_GetObjectItem(multinet_model, "threshold");
        cJSON* commands = cJSON_GetObjectItem(multinet_model, "commands");
        if (cJSON_IsString(language)) {
            language_ = language->valuestring;
        }
        if (cJSON_IsNumber(duration)) {
            duration_ = duration->valueint;
        }
        if (cJSON_IsNumber(threshold)) {
            threshold_ = threshold->valuedouble;
        }
        if (cJSON_IsArray(commands)) {
            for (int i = 0; i < cJSON_GetArraySize(commands); i++) {
                cJSON* command = cJSON_GetArrayItem(commands, i);
                if (cJSON_IsObject(command)) {
                    cJSON* command_name = cJSON_GetObjectItem(command, "command");
                    cJSON* text = cJSON_GetObjectItem(command, "text");
                    cJSON* action = cJSON_GetObjectItem(command, "action");
                    if (cJSON_IsString(command_name) && cJSON_IsString(text) && cJSON_IsString(action)) {
                        commands_.push_back({command_name->valuestring, text->valuestring, action->valuestring});
                        ESP_LOGI(TAG, "Command: %s, Text: %s, Action: %s", command_name->valuestring, text->valuestring, action->valuestring);
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
}


bool CustomWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    commands_.clear();

    if (models_list == nullptr) {
        language_ = "cn";
        models_ = esp_srmodel_init("model");
#ifdef CONFIG_CUSTOM_WAKE_WORD
        threshold_ = CONFIG_CUSTOM_WAKE_WORD_THRESHOLD / 100.0f;
        commands_.push_back({CONFIG_CUSTOM_WAKE_WORD, CONFIG_CUSTOM_WAKE_WORD_DISPLAY, "wake"});
#endif
    } else {
        models_ = models_list;
        ParseWakenetModelConfig();
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }

    // 初始化 multinet (命令词识别)
    mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, language_.c_str());
    if (mn_name_ == nullptr) {
        ESP_LOGW(TAG, "Language '%s' multinet not found, falling back to any multinet model", language_.c_str());
        mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, NULL);
    }
    if (mn_name_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize multinet, mn_name is nullptr");
        ESP_LOGI(TAG, "Please refer to https://pcn7cs20v8cr.feishu.cn/wiki/CpQjwQsCJiQSWSkYEvrcxcbVnwh to add custom wake word");
        return false;
    }

    multinet_ = esp_mn_handle_from_name(mn_name_);
    multinet_model_data_ = multinet_->create(mn_name_, duration_);
    multinet_->set_det_threshold(multinet_model_data_, threshold_);
    esp_mn_commands_clear();
    for (int i = 0; i < commands_.size(); i++) {
        esp_mn_commands_add(i + 1, commands_[i].command.c_str());
    }
    esp_mn_commands_update();

    multinet_->print_active_speech_commands(multinet_model_data_);
    return true;
}

void CustomWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void CustomWakeWord::Start() {
    running_ = true;
}

void CustomWakeWord::Stop() {
    running_ = false;
}

void CustomWakeWord::Feed(const std::vector<int16_t>& data) {
    if (multinet_model_data_ == nullptr || !running_) {
        return;
    }

    esp_mn_state_t mn_state;
    // If input channels is 2, we need to fetch the left channel data
    if (codec_->input_channels() == 2) {
        auto mono_data = std::vector<int16_t>(data.size() / 2);
        for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
            mono_data[i] = data[j];
        }

        mn_state = multinet_->detect(multinet_model_data_, const_cast<int16_t*>(mono_data.data()));
    } else {
        mn_state = multinet_->detect(multinet_model_data_, const_cast<int16_t*>(data.data()));
    }

    if (mn_state == ESP_MN_STATE_DETECTING) {
        return;
    } else if (mn_state == ESP_MN_STATE_DETECTED) {
        esp_mn_results_t *mn_result = multinet_->get_results(multinet_model_data_);
        for (int i = 0; i < mn_result->num && running_; i++) {
            ESP_LOGI(TAG, "Custom wake word detected: command_id=%d, string=%s, prob=%f",
                    mn_result->command_id[i], mn_result->string, mn_result->prob[i]);
            auto& command = commands_[mn_result->command_id[i] - 1];
            if (command.action == "wake") {
                last_detected_wake_word_ = command.text;
                running_ = false;

                if (wake_word_detected_callback_) {
                    wake_word_detected_callback_(last_detected_wake_word_);
                }
            }
        }
        multinet_->clean(multinet_model_data_);
    } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
        ESP_LOGD(TAG, "Command word detection timeout, cleaning state");
        multinet_->clean(multinet_model_data_);
    }
}

size_t CustomWakeWord::GetFeedSize() {
    if (multinet_model_data_ == nullptr) {
        return 0;
    }
    return multinet_->get_samp_chunksize(multinet_model_data_);
}
