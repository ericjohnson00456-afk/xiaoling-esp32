#include "xiaoling_client.h"

#include "assets/lang_config.h"
#include "board.h"
#include "cloud/cloud_websocket.h"
#include "system_info.h"
#include "xiaoling_auth.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <opus.h>

#include <algorithm>
#include <cstring>
#include <utility>

#define TAG "XiaolingClient"

namespace {
constexpr int kChannelTimeoutSeconds = 120;
constexpr int kProtocolConnectTimeoutMs = 10000;
constexpr int kSessionStartTimeoutMs = 3000;
constexpr int kOpusWbSampleRate = 16000;
constexpr int kOpusWbFrameDurationMs = 20;
constexpr size_t kOpusWbPayloadBytes = 40;
constexpr EventBits_t kProtocolConnectedBit = BIT0;
constexpr EventBits_t kSessionStartedBit = BIT1;

std::string Base64Encode(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    const size_t capacity = 4 * ((input.size() + 2) / 3) + 1;
    std::string output(capacity, '\0');
    size_t output_length = 0;
    const auto result = mbedtls_base64_encode(
        reinterpret_cast<unsigned char*>(output.data()), output.size(), &output_length,
        reinterpret_cast<const unsigned char*>(input.data()), input.size());
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to encode lschat connection parameters: -0x%x", -result);
        return {};
    }
    output.resize(output_length);
    return output;
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

std::string ReadRequestId(const cJSON* root) {
    const auto* rid = cJSON_GetObjectItemCaseSensitive(root, "rid");
    if (cJSON_IsString(rid) && rid->valuestring != nullptr) {
        return rid->valuestring;
    }
    if (cJSON_IsNumber(rid)) {
        return std::to_string(static_cast<unsigned long>(rid->valuedouble));
    }
    return {};
}

const char* ReadString(const cJSON* object, const char* name) {
    if (!cJSON_IsObject(object)) {
        return "-";
    }
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "-";
}

std::string ReadScalar(const cJSON* object, const char* name) {
    if (!cJSON_IsObject(object)) {
        return {};
    }

    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    if (cJSON_IsNumber(item)) {
        return std::to_string(item->valueint);
    }
    return {};
}

std::string ReadErrorDetail(const cJSON* root) {
    static constexpr const char* kDetailNames[] = {"message", "msg", "desc"};
    const auto* data = cJSON_IsObject(root)
                           ? cJSON_GetObjectItemCaseSensitive(root, "data")
                           : nullptr;
    for (const auto* owner : {root, data}) {
        for (const auto* name : kDetailNames) {
            auto detail = ReadScalar(owner, name);
            if (!detail.empty()) {
                // 限制云端文本长度，避免异常帧刷屏。
                constexpr size_t kMaxDetailBytes = 256;
                if (detail.size() > kMaxDetailBytes) {
                    detail.resize(kMaxDetailBytes);
                    detail += "...";
                }
                return detail;
            }
        }
    }
    return {};
}

void LogTextFrame(const char* direction, const std::string& text, const cJSON* parsed_root = nullptr) {
    cJSON* owned_root = nullptr;
    const cJSON* root = parsed_root;
    if (root == nullptr) {
        owned_root = cJSON_ParseWithLength(text.data(), text.size());
        root = owned_root;
    }

    if (!cJSON_IsObject(root)) {
        ESP_LOGI(TAG, "WS %s text: non-JSON, bytes=%u", direction,
                 static_cast<unsigned>(text.size()));
        cJSON_Delete(owned_root);
        return;
    }

    const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const auto* method_owner = cJSON_IsObject(data) ? data : root;
    const auto rid = ReadRequestId(root);
    ESP_LOGI(TAG,
             "WS %s text: action=%s, rid=%s, sub=%s, method=%s, code=%s, bytes=%u",
             direction,
             ReadString(root, "action"),
             rid.empty() ? "-" : rid.c_str(),
             ReadString(data, "sub"),
             ReadString(method_owner, "method"),
             ReadString(root, "code"),
             static_cast<unsigned>(text.size()));

    // 输出完整文本帧。按固定大小分片，避免较大的 MCP tools/list 或云端响应被串口日志截断。
    constexpr size_t kLogChunkBytes = 512;
    for (size_t offset = 0, chunk = 0; offset < text.size();
         offset += kLogChunkBytes, ++chunk) {
        const size_t length = std::min(kLogChunkBytes, text.size() - offset);
        ESP_LOGI(TAG, "WS %s text payload[%u]: %.*s", direction,
                 static_cast<unsigned>(chunk), static_cast<int>(length),
                 text.data() + offset);
    }
    cJSON_Delete(owned_root);
}

} // namespace

XiaolingClient::XiaolingClient() {
    event_group_ = xEventGroupCreate();
    last_incoming_time_ = std::chrono::steady_clock::now();
}

XiaolingClient::~XiaolingClient() {
    CloseAudioChannel();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

bool XiaolingClient::Start() {
    return EnsureAuthenticated();
}

void XiaolingClient::SetNarrowbandMode(bool enabled) {
    narrowband_mode_ = enabled;
}

void XiaolingClient::OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket>)> callback) {
    on_incoming_audio_ = std::move(callback);
}

void XiaolingClient::OnIncomingJson(std::function<void(const cJSON*)> callback) {
    on_incoming_json_ = std::move(callback);
}

void XiaolingClient::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = std::move(callback);
}

void XiaolingClient::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = std::move(callback);
}

void XiaolingClient::OnNetworkError(std::function<void(const std::string&)> callback) {
    on_network_error_ = std::move(callback);
}

void XiaolingClient::OnConnected(std::function<void()> callback) {
    on_connected_ = std::move(callback);
}

void XiaolingClient::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = std::move(callback);
}

void XiaolingClient::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

bool XiaolingClient::IsTimeout() const {
    const auto now = std::chrono::steady_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    if (duration.count() > kChannelTimeoutSeconds) {
        ESP_LOGE(TAG, "WebSocket channel timeout: %ld seconds", static_cast<long>(duration.count()));
        return true;
    }
    return false;
}

bool XiaolingClient::EnsureAuthenticated() {
    const auto cached_auth = XiaolingAuth::GetInstance().Load();
    if (cached_auth.IsValid()) {
        ESP_LOGI(TAG, "lschat authentication ready: source=cached, token_bytes=%u",
                 static_cast<unsigned>(cached_auth.access_token.size()));
        return true;
    }

    if (SystemInfo::GetSecretId().empty()) {
        ESP_LOGE(TAG, "CONFIG_LSCHAT_SECRET_ID is empty");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "Authenticating with lschat service");
    if (!XiaolingAuth::GetInstance().Authenticate(
            CONFIG_LSCHAT_TOKEN_URL,
            SystemInfo::GetProductId(),
            SystemInfo::GetCloudDeviceId(),
            SystemInfo::GetSecretId())) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    const auto refreshed_auth = XiaolingAuth::GetInstance().Load();
    ESP_LOGI(TAG, "lschat authentication ready: source=refreshed, token_bytes=%u",
             static_cast<unsigned>(refreshed_auth.access_token.size()));
    return true;
}

std::string XiaolingClient::BuildWebSocketUrl() const {
    cJSON* params = cJSON_CreateObject();
    if (params == nullptr) {
        return {};
    }
    cJSON_AddStringToObject(params, "scene", "main");
    cJSON_AddBoolToObject(params, "mcp", true);
    cJSON_AddStringToObject(params, "tool_protocol_version", "v2");
    cJSON_AddStringToObject(params, "type", "fullduplex");

    cJSON* firmware_info = cJSON_CreateObject();
    if (firmware_info != nullptr) {
        cJSON_AddStringToObject(firmware_info, "type", BOARD_TYPE);
        cJSON_AddStringToObject(firmware_info, "version", esp_app_get_description()->version);
        cJSON_AddItemToObject(params, "firmware_info", firmware_info);
    }

    const auto params_json = JsonToString(params);
    cJSON_Delete(params);
    ESP_LOGI(TAG, "WebSocket connection params: %s", params_json.c_str());
    const auto encoded_params = Base64Encode(params_json);
    if (encoded_params.empty()) {
        return {};
    }

    std::string base_url = CONFIG_LSCHAT_WEBSOCKET_URL;
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    return base_url + "/v1/interaction?param=" + encoded_params;
}

bool XiaolingClient::OpenAudioChannel() {
    ESP_LOGI(TAG, "Opening lschat WebSocket audio channel");
    if (!EnsureAuthenticated()) {
        return false;
    }

    CloseAudioChannel();
    error_occurred_ = false;
    protocol_connected_ = false;
    session_active_ = false;
    audio_upload_enabled_ = false;
    pending_wake_audio_.clear();
    last_incoming_time_ = std::chrono::steady_clock::now();
    if (event_group_ == nullptr) {
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    xEventGroupClearBits(event_group_, kProtocolConnectedBit | kSessionStartedBit);

    const auto websocket_url = BuildWebSocketUrl();
    if (websocket_url.empty()) {
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    websocket_ = std::make_unique<CloudWebSocket>(1);
    websocket_->OnText([this](std::string text) {
        HandleIncomingText(text);
    });
    websocket_->OnBinary([this](std::vector<uint8_t> payload) {
        HandleIncomingBinary(std::move(payload));
    });
    websocket_->OnConnected([this]() {
        ESP_LOGI(TAG, "lschat WebSocket transport connected");
    });
    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "lschat WebSocket disconnected");
        session_active_ = false;
        audio_upload_enabled_ = false;
        protocol_connected_ = false;
        if (event_group_ != nullptr) {
            xEventGroupSetBits(event_group_, kProtocolConnectedBit | kSessionStartedBit);
        }
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });
    websocket_->OnError([this](int error) {
        ESP_LOGE(TAG, "WebSocket transport error: %d", error);
        if (event_group_ != nullptr) {
            xEventGroupSetBits(event_group_, kProtocolConnectedBit | kSessionStartedBit);
        }
        SetError(Lang::Strings::SERVER_ERROR);
    });

    const auto auth = XiaolingAuth::GetInstance().Load();
    ESP_LOGI(TAG, "Preparing WebSocket Authorization header: token hidden, token_bytes=%u",
             static_cast<unsigned>(auth.access_token.size()));
    const CloudWebSocketHeaders headers = {
        {"Authorization", XiaolingAuth::NormalizeBearerToken(auth.access_token)},
        {"Device-Id", SystemInfo::GetCloudDeviceId()},
        {"Client-Id", Board::GetInstance().GetUuid()},
    };
    if (!websocket_->Connect(websocket_url, headers)) {
        // 握手失败时无法从底层拿到 HTTP 状态码，清除缓存令牌以便下次重新鉴权。
        XiaolingAuth::GetInstance().Clear();
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        websocket_.reset();
        return false;
    }

    const auto bits = xEventGroupWaitBits(event_group_, kProtocolConnectedBit, pdTRUE, pdFALSE,
                                          pdMS_TO_TICKS(kProtocolConnectTimeoutMs));
    if (!(bits & kProtocolConnectedBit) || !protocol_connected_) {
        ESP_LOGE(TAG, "Timed out waiting for lschat connected frame");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        websocket_->Disconnect();
        websocket_.reset();
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void XiaolingClient::CloseAudioChannel() {
    protocol_connected_ = false;
    session_active_ = false;
    audio_upload_enabled_ = false;
    pending_wake_word_.clear();
    pending_wake_audio_.clear();
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kProtocolConnectedBit | kSessionStartedBit);
    }
    if (websocket_ != nullptr) {
        websocket_->Disconnect();
        websocket_.reset();
    }
}

bool XiaolingClient::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && protocol_connected_ &&
           !error_occurred_ && !IsTimeout();
}

bool XiaolingClient::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (packet == nullptr || packet->payload.empty() ||
        websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    // 唤醒词音频在 start 之前由上层送入，先缓存到服务端确认 started。
    if (!audio_upload_enabled_) {
        if (!pending_wake_word_.empty()) {
            pending_wake_audio_.push_back(std::move(packet));
            return true;
        }
        return false;
    }

    if (!CheckUplinkAudioFormat(*packet)) {
        return false;
    }
    return SendUplinkAudioPacket(*packet);
}

bool XiaolingClient::SendText(const std::string& text) {
    if (text.empty() || websocket_ == nullptr || !websocket_->IsConnected()) {
        ESP_LOGW(TAG, "WS TX text skipped: transport is disconnected or message is empty");
        return false;
    }
    LogTextFrame("TX", text);
    if (!websocket_->SendText(text)) {
        ESP_LOGE(TAG, "WS TX text failed");
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

void XiaolingClient::SendWakeWordDetected(const std::string& wake_word) {
    // lschat 没有独立的唤醒词帧，唤醒词随下一次 start 的 wake_properties 发送。
    pending_wake_audio_.clear();
    pending_wake_word_ = wake_word;
}

std::string XiaolingClient::BuildStartMessage(ListeningMode mode) {
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
    cJSON_AddStringToObject(params, "aue", CONFIG_LSCHAT_UPLINK_AUE);

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

void XiaolingClient::SendStartListening(ListeningMode mode) {
    if (session_active_) {
        SendText("{\"action\":\"cancel\"}");
    }

    listening_mode_ = mode;
    audio_upload_enabled_ = false;
    uplink_audio_format_checked_ = false;
    if (event_group_ == nullptr) {
        SetError(Lang::Strings::SERVER_ERROR);
        return;
    }
    xEventGroupClearBits(event_group_, kSessionStartedBit);
    const auto message = BuildStartMessage(mode);
    session_active_ = SendText(message);
    if (!session_active_) {
        pending_wake_audio_.clear();
        return;
    }

    // 与 lschat 的 session_voice_start 一致，收到 started 后才发送音频。
    const auto bits = xEventGroupWaitBits(event_group_, kSessionStartedBit, pdTRUE, pdFALSE,
                                          pdMS_TO_TICKS(kSessionStartTimeoutMs));
    if (!(bits & kSessionStartedBit) || !audio_upload_enabled_) {
        ESP_LOGE(TAG, "Timed out waiting for lschat started frame");
        session_active_ = false;
        pending_wake_audio_.clear();
        if (!error_occurred_) {
            SetError(Lang::Strings::SERVER_TIMEOUT);
        }
        return;
    }
    if (!FlushPendingWakeAudio()) {
        SetError(Lang::Strings::SERVER_ERROR);
    }
}

void XiaolingClient::SendStopListening() {
    if (session_active_ && SendText("{\"action\":\"end\"}")) {
        audio_upload_enabled_ = false;
    }
}

void XiaolingClient::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    if (session_active_ && SendText("{\"action\":\"cancel\"}")) {
        session_active_ = false;
        audio_upload_enabled_ = false;
    }
}

void XiaolingClient::SendMcpMessage(const std::string& message) {
    cJSON* root = cJSON_ParseWithLength(message.data(), message.size());
    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "Invalid MCP response JSON");
        cJSON_Delete(root);
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, "action");
    cJSON_AddStringToObject(root, "action", "mcp");
    const auto text = JsonToString(root);
    cJSON_Delete(root);
    SendText(text);
}

bool XiaolingClient::FlushPendingWakeAudio() {
    while (!pending_wake_audio_.empty()) {
        auto packet = std::move(pending_wake_audio_.front());
        pending_wake_audio_.pop_front();
        if (packet == nullptr || packet->payload.empty()) {
            continue;
        }
        if (!CheckUplinkAudioFormat(*packet)) {
            pending_wake_audio_.clear();
            return false;
        }
        if (!SendUplinkAudioPacket(*packet)) {
            pending_wake_audio_.clear();
            return false;
        }
    }
    return true;
}

bool XiaolingClient::CheckUplinkAudioFormat(const AudioStreamPacket& packet) {
    if (uplink_audio_format_checked_) {
        return true;
    }

    if (std::strcmp(CONFIG_LSCHAT_UPLINK_AUE, "raw") == 0) {
        if (packet.sample_rate != 16000 || packet.payload.size() % sizeof(int16_t) != 0) {
            ESP_LOGE(TAG,
                     "上行 PCM 校验失败: aue=raw, sample_rate=%d, bytes=%u",
                     packet.sample_rate, static_cast<unsigned>(packet.payload.size()));
            return false;
        }

        const size_t sample_count = packet.payload.size() / sizeof(int16_t);
        const size_t duration_ms = sample_count * 1000 / packet.sample_rate;
        ESP_LOGI(TAG,
                 "上行 PCM 已校验: aue=raw, sample_rate=%d, bits=16, channels=1, samples=%u, duration=%u ms, bytes=%u",
                 packet.sample_rate, static_cast<unsigned>(sample_count),
                 static_cast<unsigned>(duration_ms),
                 static_cast<unsigned>(packet.payload.size()));
        uplink_audio_format_checked_ = true;
        return true;
    }

    const int frame_count = opus_packet_get_nb_frames(packet.payload.data(), packet.payload.size());
    const int sample_count = opus_packet_get_nb_samples(
        packet.payload.data(), packet.payload.size(), packet.sample_rate);
    if (frame_count < 1 || sample_count < 1 || packet.sample_rate <= 0) {
        ESP_LOGE(TAG, "上行音频校验失败: aue=%s, payload 不是合法 Opus 包, bytes=%u",
                 CONFIG_LSCHAT_UPLINK_AUE, static_cast<unsigned>(packet.payload.size()));
        return false;
    }

    const int duration_ms = sample_count * 1000 / packet.sample_rate;
    if (std::strcmp(CONFIG_LSCHAT_UPLINK_AUE, "opus-wb") == 0 &&
        (packet.sample_rate != kOpusWbSampleRate ||
         duration_ms != kOpusWbFrameDurationMs ||
         packet.payload.size() != kOpusWbPayloadBytes)) {
        ESP_LOGE(TAG,
                 "上行 opus-wb 校验失败: 期望 16 kHz/20 ms/40 bytes，实际 %d Hz/%d ms/%u bytes",
                 packet.sample_rate, duration_ms, static_cast<unsigned>(packet.payload.size()));
        return false;
    }
    ESP_LOGI(TAG,
             "上行 Opus 已校验: aue=%s, sample_rate=%d, channels=1, frames=%d, samples=%d, duration=%d ms, bytes=%u",
             CONFIG_LSCHAT_UPLINK_AUE, packet.sample_rate, frame_count, sample_count,
             duration_ms, static_cast<unsigned>(packet.payload.size()));
    uplink_audio_format_checked_ = true;
    return true;
}

bool XiaolingClient::SendUplinkAudioPacket(const AudioStreamPacket& packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    if (std::strcmp(CONFIG_LSCHAT_UPLINK_AUE, "opus-wb") != 0) {
        return websocket_->SendBinary(packet.payload.data(), packet.payload.size());
    }

    // lschat 的 opus-wb 是讯飞帧格式：2 字节大端 payload 长度 + Opus payload。
    if (packet.payload.size() > 0xffff) {
        ESP_LOGE(TAG, "opus-wb 帧过大: bytes=%u", static_cast<unsigned>(packet.payload.size()));
        return false;
    }
    std::vector<uint8_t> framed(packet.payload.size() + 2);
    framed[0] = static_cast<uint8_t>(packet.payload.size() >> 8);
    framed[1] = static_cast<uint8_t>(packet.payload.size() & 0xff);
    std::memcpy(framed.data() + 2, packet.payload.data(), packet.payload.size());
    return websocket_->SendBinary(framed.data(), framed.size());
}

bool XiaolingClient::IsCurrentSessionMessage(const cJSON* root) const {
    const auto rid = ReadRequestId(root);
    return rid.empty() || session_id_.empty() || rid == session_id_;
}

void XiaolingClient::HandleIncomingBinary(std::vector<uint8_t> payload) {
    last_incoming_time_ = std::chrono::steady_clock::now();
    if (on_incoming_audio_ != nullptr) {
        on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
            .sample_rate = server_sample_rate_,
            .frame_duration = server_frame_duration_,
            .timestamp = 0,
            .payload = std::move(payload),
        }));
    }
}

void XiaolingClient::HandleIncomingText(const std::string& text) {
    last_incoming_time_ = std::chrono::steady_clock::now();
    cJSON* root = cJSON_ParseWithLength(text.data(), text.size());
    if (root == nullptr) {
        LogTextFrame("RX", text);
        ESP_LOGE(TAG, "Invalid lschat JSON message");
        return;
    }
    LogTextFrame("RX", text, root);

    const auto* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (cJSON_IsString(action) && action->valuestring != nullptr && IsCurrentSessionMessage(root)) {
        if (std::strcmp(action->valuestring, "connected") == 0) {
            protocol_connected_ = true;
            ESP_LOGI(TAG, "lschat protocol connected frame accepted");
            if (event_group_ != nullptr) {
                xEventGroupSetBits(event_group_, kProtocolConnectedBit);
            }
            if (on_connected_ != nullptr) {
                on_connected_();
            }
        } else if (std::strcmp(action->valuestring, "started") == 0) {
            session_active_ = true;
            audio_upload_enabled_ = true;
            ESP_LOGI(TAG, "lschat session started: audio upload enabled");
            if (event_group_ != nullptr) {
                xEventGroupSetBits(event_group_, kSessionStartedBit);
            }
        } else if (std::strcmp(action->valuestring, "finish") == 0 ||
                   std::strcmp(action->valuestring, "closed") == 0) {
            session_active_ = false;
            audio_upload_enabled_ = false;
            if (event_group_ != nullptr) {
                xEventGroupSetBits(event_group_, kSessionStartedBit);
            }
        } else if (std::strcmp(action->valuestring, "error") == 0) {
            session_active_ = false;
            audio_upload_enabled_ = false;
            if (event_group_ != nullptr) {
                xEventGroupSetBits(event_group_, kSessionStartedBit);
            }
            const auto code = ReadScalar(root, "code");
            const auto detail = ReadErrorDetail(root);
            ESP_LOGE(TAG, "lschat 云端错误: code=%s, detail=%s",
                     code.empty() ? "unknown" : code.c_str(),
                     detail.empty() ? "-" : detail.c_str());
            if (code == "401") {
                XiaolingAuth::GetInstance().Clear();
            }
            SetError(Lang::Strings::SERVER_ERROR);
        } else if (std::strcmp(action->valuestring, "result") == 0 &&
                   listening_mode_ != kListeningModeRealtime) {
            const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            const auto* sub = cJSON_IsObject(data)
                                  ? cJSON_GetObjectItemCaseSensitive(data, "sub")
                                  : nullptr;
            if (cJSON_IsString(sub) && sub->valuestring != nullptr &&
                std::strcmp(sub->valuestring, "vad") == 0) {
                audio_upload_enabled_ = false;
            }
        }
    }

    if (on_incoming_json_ != nullptr) {
        on_incoming_json_(root);
    }
    cJSON_Delete(root);
}
