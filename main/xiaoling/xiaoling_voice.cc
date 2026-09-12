#include "xiaoling_voice.h"

#include "system/system_info.h"
#include "xiaoling_events.h"
#include "xiaoling_config.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <opus.h>

#include <cstring>
#include <utility>

#define TAG "XiaolingVoice"

namespace {

const char* ReadString(const cJSON* object, const char* name) {
    if (!cJSON_IsObject(object)) {
        return nullptr;
    }
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : nullptr;
}

bool ReadBool(const cJSON* object, const char* name) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, name));
}

std::string JsonToString(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (text == nullptr) {
        return {};
    }
    std::string result(text);
    cJSON_free(text);
    return result;
}

bool DecodeBase64Text(const char* encoded_text, std::string& decoded_text) {
    if (encoded_text == nullptr || encoded_text[0] == '\0') {
        return false;
    }

    const auto* encoded = reinterpret_cast<const unsigned char*>(encoded_text);
    const size_t encoded_size = std::strlen(encoded_text);
    size_t required_size = 0;
    const int size_result = mbedtls_base64_decode(nullptr, 0, &required_size, encoded, encoded_size);
    if (size_result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || required_size == 0) {
        return false;
    }

    std::vector<unsigned char> decoded(required_size + 1, 0);
    size_t decoded_size = 0;
    if (mbedtls_base64_decode(decoded.data(), required_size, &decoded_size,
                              encoded, encoded_size) != 0 || decoded_size == 0) {
        return false;
    }
    if (decoded[decoded_size - 1] == '\0') {
        --decoded_size;
    }
    if (decoded_size == 0 || std::memchr(decoded.data(), '\0', decoded_size) != nullptr) {
        return false;
    }
    decoded_text.assign(reinterpret_cast<const char*>(decoded.data()), decoded_size);
    return true;
}

}  // 匿名命名空间

XiaolingVoice::XiaolingVoice() {
    event_group_ = xEventGroupCreate();
}

XiaolingVoice::~XiaolingVoice() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

void XiaolingVoice::SetTransport(TextSender text_sender,
                                 BinarySender binary_sender,
                                 SessionStateCallback session_state_callback,
                                 ErrorCallback error_callback) {
    text_sender_ = std::move(text_sender);
    binary_sender_ = std::move(binary_sender);
    session_state_callback_ = std::move(session_state_callback);
    error_callback_ = std::move(error_callback);
}

void XiaolingVoice::SetNarrowbandMode(bool enabled) {
    narrowband_mode_ = enabled;
}

void XiaolingVoice::OnIncomingAudio(IncomingAudioCallback callback) {
    on_incoming_audio_ = std::move(callback);
}

bool XiaolingVoice::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (packet == nullptr || packet->payload.empty() || !binary_sender_) {
        return false;
    }

    // 唤醒音频在 start 之前到达，先缓存到服务端确认 started。
    if (!audio_upload_enabled_) {
        if (!pending_wake_word_.empty()) {
            pending_wake_audio_.push_back(std::move(packet));
            return true;
        }
        return false;
    }

    return CheckUplinkAudioFormat(*packet) && SendUplinkAudioPacket(*packet);
}

void XiaolingVoice::SendWakeWordDetected(const std::string& wake_word) {
    // lschat 没有独立唤醒词帧，唤醒词随下一次 start 的 wake_properties 发送。
    pending_wake_audio_.clear();
    pending_wake_word_ = wake_word;
}

std::string XiaolingVoice::BuildStartMessage(ListeningMode mode) {
    session_id_ = std::to_string(static_cast<unsigned long>(esp_random()));

    cJSON* root = cJSON_CreateObject();
    cJSON* params = cJSON_CreateObject();
    if (root == nullptr || params == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(params);
        return {};
    }
    cJSON_AddStringToObject(root, "action", "start");
    cJSON_AddItemToObject(root, "params", params);

    if (mode == kListeningModeRealtime) {
        cJSON_AddStringToObject(params, "fullduplex", "1");
    }
    cJSON_AddStringToObject(params, "data_type", "audio");
    cJSON_AddStringToObject(params, "aue", XiaolingConfig::kUplinkAudioEncoding);

    cJSON* features = cJSON_CreateArray();
    cJSON_AddItemToArray(features, cJSON_CreateString("nlu"));
    cJSON_AddItemToArray(features, cJSON_CreateString("tts"));
    cJSON_AddItemToObject(params, "features", features);

    cJSON* nlu_properties = cJSON_CreateObject();
    cJSON* custom = cJSON_CreateObject();
    cJSON_AddStringToObject(custom, "rid", session_id_.c_str());
    cJSON_AddStringToObject(nlu_properties, "sn", SystemInfo::GetCloudDeviceId().c_str());
    cJSON_AddItemToObject(nlu_properties, "custom", custom);
    cJSON_AddItemToObject(params, "nlu_properties", nlu_properties);

    cJSON* asr_properties = cJSON_CreateObject();
    cJSON_AddStringToObject(asr_properties, "audio_gain", "3.0");
    if (mode == kListeningModeManualStop) {
        cJSON_AddStringToObject(asr_properties, "evad", "0");
        cJSON_AddStringToObject(asr_properties, "svad", "0");
    }
    if (!pending_wake_word_.empty() && mode != kListeningModeRealtime) {
        cJSON_AddStringToObject(asr_properties, "oneshot", "1");
    }
    cJSON_AddItemToObject(params, "asr_properties", asr_properties);

    if (!pending_wake_word_.empty()) {
        cJSON* wake_properties = cJSON_CreateObject();
        cJSON* words = cJSON_CreateArray();
        cJSON_AddItemToArray(words, cJSON_CreateString(pending_wake_word_.c_str()));
        cJSON_AddItemToObject(wake_properties, "words", words);
        cJSON_AddItemToObject(params, "wake_properties", wake_properties);
    }

    const auto text = JsonToString(root);
    cJSON_Delete(root);
    pending_wake_word_.clear();
    return text;
}

void XiaolingVoice::SendStartListening(ListeningMode mode) {
    if (!text_sender_) {
        if (error_callback_) {
            error_callback_(XiaolingVoiceError::kServerError);
        }
        return;
    }
    if (session_active_) {
        text_sender_("{\"action\":\"cancel\"}");
    }

    listening_mode_ = mode;
    audio_upload_enabled_ = false;
    uplink_audio_format_checked_ = false;
    if (event_group_ == nullptr) {
        if (error_callback_) {
            error_callback_(XiaolingVoiceError::kServerError);
        }
        return;
    }

    xEventGroupClearBits(event_group_, kSessionStartedBit);
    SetSessionActive(text_sender_(BuildStartMessage(mode)));
    if (!session_active_) {
        pending_wake_audio_.clear();
        return;
    }

    // 收到 started 后才允许发送音频。
    const auto bits = xEventGroupWaitBits(event_group_, kSessionStartedBit,
                                          pdTRUE, pdFALSE,
                                          pdMS_TO_TICKS(kSessionStartTimeoutMs));
    if (!(bits & kSessionStartedBit) || !audio_upload_enabled_) {
        ESP_LOGE(TAG, "等待 lschat started 消息超时");
        SetSessionActive(false);
        pending_wake_audio_.clear();
        if (error_callback_) {
            error_callback_(XiaolingVoiceError::kServerTimeout);
        }
        return;
    }
    if (!FlushPendingWakeAudio() && error_callback_) {
        error_callback_(XiaolingVoiceError::kServerError);
    }
}

void XiaolingVoice::SendStopListening() {
    if (session_active_ && text_sender_ && text_sender_("{\"action\":\"end\"}")) {
        audio_upload_enabled_ = false;
    }
}

void XiaolingVoice::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    if (session_active_ && text_sender_ && text_sender_("{\"action\":\"cancel\"}")) {
        audio_upload_enabled_ = false;
        SetSessionActive(false);
    }
}

bool XiaolingVoice::FlushPendingWakeAudio() {
    while (!pending_wake_audio_.empty()) {
        auto packet = std::move(pending_wake_audio_.front());
        pending_wake_audio_.pop_front();
        if (packet == nullptr || packet->payload.empty()) {
            continue;
        }
        if (!CheckUplinkAudioFormat(*packet) || !SendUplinkAudioPacket(*packet)) {
            pending_wake_audio_.clear();
            return false;
        }
    }
    return true;
}

bool XiaolingVoice::CheckUplinkAudioFormat(const AudioStreamPacket& packet) {
    if (uplink_audio_format_checked_) {
        return true;
    }

    if (std::strcmp(XiaolingConfig::kUplinkAudioEncoding, "raw") == 0) {
        if (packet.sample_rate != kServerSampleRate ||
            packet.payload.size() % sizeof(int16_t) != 0) {
            ESP_LOGE(TAG, "上行 PCM 校验失败: sample_rate=%d, bytes=%u",
                     packet.sample_rate, static_cast<unsigned>(packet.payload.size()));
            return false;
        }
        uplink_audio_format_checked_ = true;
        return true;
    }

    const int frame_count = opus_packet_get_nb_frames(packet.payload.data(), packet.payload.size());
    const int sample_count = opus_packet_get_nb_samples(
        packet.payload.data(), packet.payload.size(), packet.sample_rate);
    if (frame_count < 1 || sample_count < 1 || packet.sample_rate <= 0) {
        ESP_LOGE(TAG, "上行数据不是有效的 Opus 包: bytes=%u",
                 static_cast<unsigned>(packet.payload.size()));
        return false;
    }

    const int duration_ms = sample_count * 1000 / packet.sample_rate;
    if (std::strcmp(XiaolingConfig::kUplinkAudioEncoding, "opus-wb") == 0 &&
        (packet.sample_rate != kOpusWbSampleRate ||
         duration_ms != kOpusWbFrameDurationMs ||
         packet.payload.size() != kOpusWbPayloadBytes)) {
        ESP_LOGE(TAG,
                 "上行 opus-wb 校验失败: 期望 16 kHz/20 ms/40 bytes，实际 %d Hz/%d ms/%u bytes",
                 packet.sample_rate, duration_ms,
                 static_cast<unsigned>(packet.payload.size()));
        return false;
    }
    uplink_audio_format_checked_ = true;
    return true;
}

bool XiaolingVoice::SendUplinkAudioPacket(const AudioStreamPacket& packet) {
    if (!binary_sender_) {
        return false;
    }
    if (std::strcmp(XiaolingConfig::kUplinkAudioEncoding, "opus-wb") != 0) {
        return binary_sender_(packet.payload.data(), packet.payload.size());
    }

    // opus-wb 使用讯飞帧格式：2 字节大端有效载荷长度 + Opus 数据。
    if (packet.payload.size() > 0xffff) {
        ESP_LOGE(TAG, "opus-wb 帧过大: bytes=%u",
                 static_cast<unsigned>(packet.payload.size()));
        return false;
    }
    std::vector<uint8_t> framed(packet.payload.size() + 2);
    framed[0] = static_cast<uint8_t>(packet.payload.size() >> 8);
    framed[1] = static_cast<uint8_t>(packet.payload.size() & 0xff);
    std::memcpy(framed.data() + 2, packet.payload.data(), packet.payload.size());
    return binary_sender_(framed.data(), framed.size());
}

void XiaolingVoice::HandleBinary(std::vector<uint8_t> payload) {
    if (on_incoming_audio_) {
        on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
            .sample_rate = kServerSampleRate,
            .frame_duration = kServerFrameDurationMs,
            .timestamp = 0,
            .payload = std::move(payload),
        }));
    }
}

void XiaolingVoice::HandleMessage(const cJSON* root) {
    const auto* action = ReadString(root, "action");
    if (action == nullptr) {
        return;
    }

    if (std::strcmp(action, "started") == 0) {
        ResetResultState();
        SetSessionActive(true);
        audio_upload_enabled_ = true;
        if (event_group_ != nullptr) {
            xEventGroupSetBits(event_group_, kSessionStartedBit);
        }
        ESP_LOGI(TAG, "lschat 语音会话已启动，允许上传音频");
        return;
    }
    if (std::strcmp(action, "finish") == 0 ||
        std::strcmp(action, "closed") == 0 ||
        std::strcmp(action, "error") == 0) {
        audio_upload_enabled_ = false;
        SetSessionActive(false);
        if (event_group_ != nullptr) {
            xEventGroupSetBits(event_group_, kSessionStartedBit);
        }
        EndIat();
        EndTtsText();
        return;
    }
    if (std::strcmp(action, "result") != 0) {
        return;
    }

    const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const auto* sub = ReadString(data, "sub");
    if (sub == nullptr) {
        return;
    }
    if (std::strcmp(sub, "vad") == 0 && listening_mode_ != kListeningModeRealtime) {
        audio_upload_enabled_ = false;
    } else if (std::strcmp(sub, "iat") == 0) {
        HandleIat(data);
    } else if (std::strcmp(sub, "tts") == 0) {
        HandleTts(data);
    }
}

void XiaolingVoice::Reset() {
    audio_upload_enabled_ = false;
    SetSessionActive(false);
    pending_wake_word_.clear();
    pending_wake_audio_.clear();
    uplink_audio_format_checked_ = false;
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kSessionStartedBit);
    }
    ResetResultState();
}

void XiaolingVoice::SetSessionActive(bool active) {
    session_active_ = active;
    if (session_state_callback_) {
        session_state_callback_(active);
    }
}

void XiaolingVoice::ResetResultState() {
    EndIat();
    EndTtsText();
    iat_active_ = false;
    tts_text_active_ = false;
    last_iat_text_.clear();
    last_tts_text_.clear();
}

void XiaolingVoice::HandleIat(const cJSON* data) {
    const auto* text = ReadString(data, "text");
    if (text == nullptr) {
        return;
    }

    last_iat_text_ = text;
    auto& events = XiaolingEventBus::GetInstance();
    if (!iat_active_) {
        iat_active_ = true;
        events.Publish(XIAOLING_IAT_START, last_iat_text_);
    } else if (!ReadBool(data, "is_last")) {
        events.Publish(XIAOLING_IAT_UPDATE, last_iat_text_);
    }

    if (ReadBool(data, "is_last")) {
        events.Publish(XIAOLING_IAT_END, last_iat_text_);
        iat_active_ = false;
    }
}

void XiaolingVoice::HandleTts(const cJSON* data) {
    auto& events = XiaolingEventBus::GetInstance();
    if (const auto* content = ReadString(data, "content")) {
        std::string url;
        if (DecodeBase64Text(content, url)) {
            events.Publish(XIAOLING_TTS_URL, std::move(url));
        } else {
            ESP_LOGE(TAG, "小聆 TTS URL 解码失败");
        }
    }

    // TTS 文本使用 text 和 is_last 表示生命周期，同时兼容 subtitle 字段。
    const char* tts_text = ReadString(data, "text");
    if (tts_text == nullptr) {
        tts_text = ReadString(data, "subtitle");
    }
    if (tts_text != nullptr) {
        last_tts_text_ = tts_text;
        if (!tts_text_active_) {
            tts_text_active_ = true;
            events.Publish(XIAOLING_TTS_TXT_START, last_tts_text_);
        } else if (!ReadBool(data, "is_last")) {
            events.Publish(XIAOLING_TTS_TXT_UPDATE, last_tts_text_);
        }
        if (ReadBool(data, "is_last")) {
            events.Publish(XIAOLING_TTS_TXT_END, last_tts_text_);
            tts_text_active_ = false;
        }
    }

    if (ReadBool(data, "is_last")) {
        EndTtsText();
    }
}

void XiaolingVoice::EndIat() {
    if (!iat_active_) {
        return;
    }
    XiaolingEventBus::GetInstance().Publish(XIAOLING_IAT_END, last_iat_text_);
    iat_active_ = false;
}

void XiaolingVoice::EndTtsText() {
    if (tts_text_active_) {
        XiaolingEventBus::GetInstance().Publish(XIAOLING_TTS_TXT_END, last_tts_text_);
        tts_text_active_ = false;
    }
}
