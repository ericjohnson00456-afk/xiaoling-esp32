#include "xiaoling/session/session_voice.h"

#include "system/system_info.h"
#include "xiaoling/xiaoling_events.h"
#include "xiaoling/conn/xiaoling_config.h"
#include "board.h"
#include "network_interface.h"
#include "http.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include "base64_codec.h"
#include <opus.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <cctype>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>

#define TAG "XiaolingSessionVoice"

namespace {

constexpr int kSubtitleHttpWaitMs = 5000;
constexpr int kSubtitleHeaderMaxWaits = 12;

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

bool AsciiEqualsIgnoreCase(const std::string& value, const char* expected) {
    if (expected == nullptr || value.size() != std::strlen(expected)) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(expected[i]))) {
            return false;
        }
    }
    return true;
}

bool ReadUint32(const cJSON* object, const char* name, uint32_t* value) {
    if (!cJSON_IsObject(object) || value == nullptr) {
        return false;
    }
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > static_cast<double>(UINT32_MAX)) {
        return false;
    }
    const auto converted = static_cast<uint32_t>(item->valuedouble);
    if (static_cast<double>(converted) != item->valuedouble) {
        return false;
    }
    *value = converted;
    return true;
}

bool CountUtf8Codepoints(const char* text, uint32_t* count) {
    if (text == nullptr || count == nullptr) {
        return false;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(text);
    const auto* end = p + std::strlen(text);
    uint32_t total = 0;
    while (p < end) {
        uint32_t codepoint = 0;
        uint8_t continuation_count = 0;
        if (*p < 0x80) {
            codepoint = *p++;
        } else if ((*p & 0xe0) == 0xc0) {
            codepoint = *p++ & 0x1f;
            continuation_count = 1;
            if (codepoint < 2) {
                return false;
            }
        } else if ((*p & 0xf0) == 0xe0) {
            codepoint = *p++ & 0x0f;
            continuation_count = 2;
        } else if ((*p & 0xf8) == 0xf0) {
            codepoint = *p++ & 0x07;
            continuation_count = 3;
        } else {
            return false;
        }
        if (static_cast<size_t>(end - p) < continuation_count) {
            return false;
        }
        for (uint8_t i = 0; i < continuation_count; ++i) {
            if ((p[i] & 0xc0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (p[i] & 0x3f);
        }
        p += continuation_count;
        if ((continuation_count == 2 && codepoint < 0x800) ||
            (continuation_count == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint > 0x10ffff) {
            return false;
        }
        ++total;
    }
    *count = total;
    return true;
}

}  // 匿名命名空间

class TtsSubtitleFetcher final {
public:
    using Callback = std::function<void(int, const std::string&)>;

    explicit TtsSubtitleFetcher(Callback callback) : callback_(std::move(callback)) {
    }

    ~TtsSubtitleFetcher() {
        Cancel();
        std::unique_lock<std::mutex> lock(mutex_);
        task_cv_.wait(lock, [this]() { return !worker_running_; });
    }

    void Start(const std::string& url) {
        if (url.empty()) {
            return;
        }
        const uint32_t generation = generation_.fetch_add(1) + 1;
        bool start_worker = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_url_ = url;
            pending_generation_ = generation;
            if (!worker_running_) {
                worker_running_ = true;
                start_worker = true;
            }
        }
        if (!start_worker) {
            ESP_LOGI(TAG, "字幕请求已替换排队，等待旧请求退出: generation=%lu",
                     static_cast<unsigned long>(generation));
            return;
        }
        if (xTaskCreate(&TtsSubtitleFetcher::TaskEntry, "tts_subtitle", 8192,
                        this, 3, nullptr) != pdPASS) {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_running_ = false;
            pending_url_.clear();
            task_cv_.notify_all();
            ESP_LOGE(TAG, "创建字幕请求任务失败");
        }
    }

    void SetAccessToken(std::string token) {
        std::lock_guard<std::mutex> lock(mutex_);
        access_token_ = std::move(token);
    }

    void Cancel() {
        generation_.fetch_add(1);
        std::lock_guard<std::mutex> lock(mutex_);
        pending_url_.clear();
    }

private:
    enum Event : int {
        kData = 0,
        kDone = 1,
    };

    struct Request {
        std::string url;
        uint32_t generation;
    };

    static void TaskEntry(void* arg) {
        auto* owner = static_cast<TtsSubtitleFetcher*>(arg);
        if (owner != nullptr) {
            owner->WorkerLoop();
        }
        vTaskDelete(nullptr);
    }

    void WorkerLoop() {
        while (true) {
            Request request;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pending_url_.empty()) {
                    worker_running_ = false;
                    task_cv_.notify_all();
                    return;
                }
                request.url = std::move(pending_url_);
                pending_url_.clear();
                request.generation = pending_generation_;
            }
            ESP_LOGI(TAG, "字幕请求开始: generation=%lu, url=%s",
                     static_cast<unsigned long>(request.generation), request.url.c_str());
            Run(request);
        }
    }

    bool IsCurrent(uint32_t generation) const {
        return generation_.load() == generation;
    }

    void Emit(Event event, const std::string& text, uint32_t generation) {
        if (IsCurrent(generation) && callback_) {
            callback_(event, text);
        }
    }

    void ParseBlock(const std::string& block, uint32_t generation, bool* done) {
        std::string event_name;
        std::string payload;
        size_t offset = 0;
        while (offset <= block.size()) {
            const size_t end = block.find('\n', offset);
            std::string line = block.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.rfind("event:", 0) == 0) {
                event_name = line.substr(6);
                while (!event_name.empty() && event_name.front() == ' ') {
                    event_name.erase(event_name.begin());
                }
            } else if (line.rfind("data:", 0) == 0) {
                if (!payload.empty()) {
                    payload.push_back('\n');
                }
                payload += line.substr(5);
                while (!payload.empty() && payload.front() == ' ') {
                    payload.erase(payload.begin());
                }
            }
            if (end == std::string::npos) {
                break;
            }
            offset = end + 1;
        }

        // Some deployments send an empty heartbeat as `event: data`; it is
        // not a subtitle and must not terminate the stream.
        while (!payload.empty() && std::isspace(static_cast<unsigned char>(payload.front()))) {
            payload.erase(payload.begin());
        }
        while (!payload.empty() && std::isspace(static_cast<unsigned char>(payload.back()))) {
            payload.pop_back();
        }
        if (AsciiEqualsIgnoreCase(event_name, "done") ||
            AsciiEqualsIgnoreCase(event_name, "end") ||
            AsciiEqualsIgnoreCase(payload, "[done]") ||
            AsciiEqualsIgnoreCase(payload, "done") ||
            AsciiEqualsIgnoreCase(payload, "\"done\"")) {
            ESP_LOGI(TAG, "字幕流收到 done");
            Emit(kDone, {}, generation);
            *done = true;
        } else if ((event_name.empty() || event_name == "data") && !payload.empty()) {
            // 服务端同时存在标准 SSE(event:data + data:)和简化 SSE(data:)
            // 两种格式；没有 event 行时，非 [DONE] 的 data 仍然是字幕句段。
            Emit(kData, payload, generation);
        }
    }

    void ParseAvailable(std::string* buffer, uint32_t generation, bool* done) {
        while (!*done && IsCurrent(generation)) {
            size_t delimiter = buffer->find("\n\n");
            size_t delimiter_size = 2;
            const size_t crlf_delimiter = buffer->find("\r\n\r\n");
            if (crlf_delimiter != std::string::npos &&
                (delimiter == std::string::npos || crlf_delimiter < delimiter)) {
                delimiter = crlf_delimiter;
                delimiter_size = 4;
            }
            if (delimiter == std::string::npos) {
                return;
            }
            const std::string block = buffer->substr(0, delimiter);
            buffer->erase(0, delimiter + delimiter_size);
            ParseBlock(block, generation, done);
        }
    }

    void Run(const Request& request) {
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr || !IsCurrent(request.generation)) {
            return;
        }
        auto http = network->CreateHttp();
        if (http == nullptr) {
            ESP_LOGW(TAG, "创建字幕 HTTP 客户端失败");
            return;
        }
        // Bound every wait so a superseded subtitle request releases its TLS
        // connection promptly. Header and SSE idle waits are retried below
        // while this generation is still current.
        http->SetTimeout(kSubtitleHttpWaitMs);
        http->SetHeader("Accept", "text/event-stream");
        http->SetHeader("Cache-Control", "no-cache");
        http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!access_token_.empty()) {
                std::string authorization = access_token_;
                if (authorization.rfind("Bearer ", 0) != 0 &&
                    authorization.rfind("bearer ", 0) != 0) {
                    authorization = "Bearer " + authorization;
                }
                http->SetHeader("Authorization", authorization);
            }
        }
        http->SetHeader("Device-Id", SystemInfo::GetCloudDeviceId());
        http->SetHeader("Client-Id", Board::GetInstance().GetUuid());
        if (!http->Open("GET", request.url)) {
            if (IsCurrent(request.generation)) {
                ESP_LOGW(TAG, "字幕 HTTP 建连失败: %s", request.url.c_str());
            }
            http->Close();
            return;
        }
        if (!IsCurrent(request.generation)) {
            http->Close();
            return;
        }
        int status_code = -1;
        int header_waits = 0;
        while (IsCurrent(request.generation) && status_code < 0 &&
               header_waits < kSubtitleHeaderMaxWaits) {
            status_code = http->GetStatusCode();
            if (status_code < 0) {
                ++header_waits;
                ESP_LOGW(TAG, "字幕 HTTP 等待响应头: attempt=%d/%d",
                         header_waits, kSubtitleHeaderMaxWaits);
            }
        }
        if (!IsCurrent(request.generation)) {
            http->Close();
            return;
        }
        ESP_LOGI(TAG, "字幕 HTTP 响应: status=%d", status_code);
        if (status_code != 200) {
            ESP_LOGW(TAG, "字幕 HTTP 请求失败: %s", request.url.c_str());
            http->Close();
            return;
        }

        std::string buffer;
        bool done = false;
        int read_errors = 0;
        char read_buffer[1024];
        while (!done && IsCurrent(request.generation)) {
            const int bytes = http->Read(read_buffer, sizeof(read_buffer));
            if (bytes == 0) {
                ESP_LOGI(TAG, "字幕 HTTP EOF");
                break;
            }
            if (bytes < 0) {
                if (!IsCurrent(request.generation)) {
                    break;
                }
                ++read_errors;
                ESP_LOGW(TAG, "字幕 HTTP Read 返回 %d: 连续错误=%d", bytes, read_errors);
                // -1 既可能是等待超时，也可能是临时连接异常；短暂超时不能
                // 结束 SSE，否则长句的后半段会永远没有字幕。
                if (read_errors >= 60) {
                    ESP_LOGW(TAG, "字幕 HTTP 连续读取失败，结束请求");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            read_errors = 0;
            buffer.append(read_buffer, static_cast<size_t>(bytes));
            ParseAvailable(&buffer, request.generation, &done);
        }
        if (!done && IsCurrent(request.generation)) {
            ParseAvailable(&buffer, request.generation, &done);
            if (!done) {
                ESP_LOGI(TAG, "字幕请求结束: 未收到 done");
                Emit(kDone, {}, request.generation);
            }
        }
        http->Close();
    }

    Callback callback_;
    std::atomic<uint32_t> generation_{0};
    std::mutex mutex_;
    std::condition_variable task_cv_;
    bool worker_running_ = false;
    std::string pending_url_;
    uint32_t pending_generation_ = 0;
    std::string access_token_;
};

namespace {

std::string JsonToString(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (text == nullptr) {
        return {};
    }
    std::string result(text);
    cJSON_free(text);
    return result;
}

}  // 匿名命名空间

XiaolingSessionVoice::XiaolingSessionVoice()
    : subtitle_fetcher_(std::make_unique<TtsSubtitleFetcher>(
          [this](int event, const std::string& text) {
              HandleSubtitleEvent(event, text);
          })) {
    event_group_ = xEventGroupCreate();
    auto& events = XiaolingEventBus::GetInstance();
    tts_stream_ready_subscription_id_ = events.Subscribe(
        XIAOLING_TTS_STREAM_READY, [this](const XiaolingEvent&) {
            OnTtsPipelinePlaying();
        });
    tts_pipeline_subscription_id_ = events.Subscribe(
        XIAOLING_TTS_PLAYING, [this](const XiaolingEvent&) {
            OnTtsPipelinePlaying();
        });
    tts_output_started_subscription_id_ = events.Subscribe(
        XIAOLING_TTS_OUTPUT_STARTED, [this](const XiaolingEvent&) {
            OnTtsOutputStarted();
        });
    tts_progress_subscription_id_ = events.Subscribe(
        XIAOLING_TTS_OUTPUT_PROGRESS, [this](const XiaolingEvent& event) {
            if (event.text.empty()) {
                return;
            }
            char* end = nullptr;
            const unsigned long value = std::strtoul(event.text.c_str(), &end, 10);
            if (end != event.text.c_str() && *end == '\0' && value <= UINT32_MAX) {
                OnTtsAudioProgress(static_cast<uint32_t>(value));
            }
        });
    tts_stopped_subscription_id_ = events.Subscribe(
        XIAOLING_TTS_STOPPED, [this](const XiaolingEvent& event) {
            OnTtsPlaybackStopped(event.text == "natural");
        });
}

XiaolingSessionVoice::~XiaolingSessionVoice() {
    auto& events = XiaolingEventBus::GetInstance();
    if (tts_stream_ready_subscription_id_ != 0) {
        events.Unsubscribe(tts_stream_ready_subscription_id_);
    }
    if (tts_pipeline_subscription_id_ != 0) {
        events.Unsubscribe(tts_pipeline_subscription_id_);
    }
    if (tts_output_started_subscription_id_ != 0) {
        events.Unsubscribe(tts_output_started_subscription_id_);
    }
    if (tts_progress_subscription_id_ != 0) {
        events.Unsubscribe(tts_progress_subscription_id_);
    }
    if (tts_stopped_subscription_id_ != 0) {
        events.Unsubscribe(tts_stopped_subscription_id_);
    }
    OnTtsPlaybackStopped(false);
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

void XiaolingSessionVoice::SetTransport(TextSender text_sender,
                                 BinarySender binary_sender,
                                 SessionStateCallback session_state_callback,
                                 ErrorCallback error_callback) {
    text_sender_ = std::move(text_sender);
    binary_sender_ = std::move(binary_sender);
    session_state_callback_ = std::move(session_state_callback);
    error_callback_ = std::move(error_callback);
}

void XiaolingSessionVoice::SetNarrowbandMode(bool enabled) {
    narrowband_mode_ = enabled;
}

void XiaolingSessionVoice::SetAccessToken(std::string token) {
    if (subtitle_fetcher_ != nullptr) {
        subtitle_fetcher_->SetAccessToken(std::move(token));
    }
}

void XiaolingSessionVoice::OnIncomingAudio(IncomingAudioCallback callback) {
    on_incoming_audio_ = std::move(callback);
}

bool XiaolingSessionVoice::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::vector<std::unique_ptr<AudioStreamPacket>> packets;
    packets.push_back(std::move(packet));
    return SendAudioBatch(std::move(packets));
}

bool XiaolingSessionVoice::SendAudioBatch(
        std::vector<std::unique_ptr<AudioStreamPacket>> packets) {
    if (packets.empty() || !binary_sender_) {
        return false;
    }

    packets.erase(std::remove_if(packets.begin(), packets.end(),
                                 [](const auto& packet) {
                                     return packet == nullptr || packet->payload.empty();
                                 }),
                  packets.end());
    if (packets.empty()) {
        return false;
    }

    // 唤醒音频在 start 之前到达，先缓存到服务端确认 started。
    if (!audio_upload_enabled_) {
        if (!pending_wake_word_.empty()) {
            for (auto& packet : packets) {
                pending_wake_audio_.push_back(std::move(packet));
            }
            return true;
        }
        return false;
    }

    if (std::strcmp(XiaolingConfig::kUplinkAudioEncoding, "opus-wb") != 0) {
        for (const auto& packet : packets) {
            if (!CheckUplinkAudioFormat(*packet) || !SendUplinkAudioPacket(*packet)) {
                return false;
            }
        }
        return true;
    }

    std::vector<uint8_t> framed;
    framed.reserve(packets.size() * (kOpusWbPayloadBytes + 2));
    for (const auto& packet : packets) {
        if (!CheckUplinkAudioFormat(*packet) || packet->payload.size() > 0xffff) {
            return false;
        }
        framed.push_back(static_cast<uint8_t>(packet->payload.size() >> 8));
        framed.push_back(static_cast<uint8_t>(packet->payload.size() & 0xff));
        framed.insert(framed.end(), packet->payload.begin(), packet->payload.end());
    }
    return binary_sender_(framed.data(), framed.size());
}

void XiaolingSessionVoice::SendWakeWordDetected(const std::string& wake_word) {
    // 云端不处理 wake_properties；本地仅用唤醒状态缓存 start 前的音频并设置 oneshot。
    pending_wake_audio_.clear();
    pending_wake_word_ = wake_word;
}

std::string XiaolingSessionVoice::BuildStartMessage(ListeningMode mode) {
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

    cJSON_AddStringToObject(params, "fullduplex", "1");
    const int32_t timeout_ms = KvUser::GetFullDuplexTimeoutMs();
    char timeout_str[12];
    const uint32_t timeout_s = timeout_ms > 0
                                    ? static_cast<uint32_t>(timeout_ms / 1000)
                                    : 0;
    std::snprintf(timeout_str, sizeof(timeout_str), "%lu",
                  static_cast<unsigned long>(timeout_s));
    cJSON_AddStringToObject(params, "fullduplex_timeout", timeout_str);
    cJSON_AddStringToObject(params, "data_type", "audio");
    cJSON_AddStringToObject(params, "aue", XiaolingConfig::kUplinkAudioEncoding);

    cJSON* features = cJSON_CreateArray();
    cJSON_AddItemToArray(features, cJSON_CreateString("nlu"));
    cJSON_AddItemToArray(features, cJSON_CreateString("tts"));
    cJSON_AddItemToObject(params, "features", features);

    cJSON* nlu_properties = cJSON_CreateObject();
    cJSON* abilities = cJSON_CreateArray();
    cJSON* event_ability = cJSON_CreateObject();
    cJSON* event_intents = cJSON_CreateArray();
    cJSON_AddStringToObject(event_ability, "name", "event");
    cJSON_AddItemToArray(event_intents, cJSON_CreateString("reply_interrupted"));
    cJSON_AddItemToObject(event_ability, "intents", event_intents);
    cJSON_AddItemToArray(abilities, event_ability);
    cJSON_AddItemToObject(nlu_properties, "abilities", abilities);

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

    const auto text = JsonToString(root);
    cJSON_Delete(root);
    pending_wake_word_.clear();
    return text;
}

void XiaolingSessionVoice::SendStartListening(ListeningMode mode) {
    if (!text_sender_) {
        if (error_callback_) {
            error_callback_(XiaolingSessionVoiceError::kServerError);
        }
        return;
    }
    if (session_active_) {
        text_sender_("{\"action\":\"cancel\"}");
    }

    end_requested_ = false;
    audio_upload_enabled_ = false;
    uplink_audio_format_checked_ = false;
    if (event_group_ == nullptr) {
        if (error_callback_) {
            error_callback_(XiaolingSessionVoiceError::kServerError);
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
            error_callback_(XiaolingSessionVoiceError::kServerTimeout);
        }
        return;
    }
    if (!FlushPendingWakeAudio() && error_callback_) {
        error_callback_(XiaolingSessionVoiceError::kServerError);
    }
}

void XiaolingSessionVoice::SendStopListening() {
    if (session_active_ && text_sender_) {
        // Set this before sending: the WebSocket receive task may process the
        // server acknowledgement immediately on another core.
        end_requested_ = true;
        if (text_sender_("{\"action\":\"end\"}")) {
            audio_upload_enabled_ = false;
        } else {
            end_requested_ = false;
        }
    }
}

bool XiaolingSessionVoice::EndSession() {
    const bool was_active = session_active_.load();
    const bool sent = !was_active ||
        (text_sender_ && text_sender_("{\"action\":\"end\"}"));

    audio_upload_enabled_ = false;
    end_requested_ = false;
    pending_wake_word_.clear();
    pending_wake_audio_.clear();
    uplink_audio_format_checked_ = false;
    SetSessionActive(false);
    ResetResultState();
    return sent;
}

void XiaolingSessionVoice::PrepareAbortSpeaking(AbortReason reason) {
    if (reason != kAbortReasonWakeWordDetected) {
        return;
    }

    std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
    if (frozen_interruption_.valid || timeline_reported_) {
        return;
    }
    const bool tts_active = tts_audio_pending_ || tts_pipeline_running_ || tts_audio_playing_;
    if (!tts_active || !timeline_negotiated_ || !timeline_valid_ || reply_sid_.empty()) {
        ESP_LOGW(TAG,
                 "TTS 打断无可靠 timeline 游标: active=%d, negotiated=%d, valid=%d, sid=%d",
                 tts_active ? 1 : 0, timeline_negotiated_ ? 1 : 0,
                 timeline_valid_ ? 1 : 0, reply_sid_.empty() ? 0 : 1);
        return;
    }

    for (const auto& cursor : timeline_cursors_) {
        if (cursor.end_ms > tts_audio_clock_ms_) {
            frozen_interruption_.valid = true;
            frozen_interruption_.sid = reply_sid_;
            frozen_interruption_.cursor = cursor;
            frozen_interruption_.playback_position_ms = tts_audio_clock_ms_;
            timeline_reported_ = true;
            ESP_LOGI(TAG,
                     "冻结 TTS 打断点: sid=%s, clock=%lums, sentence=%lu, range=[%lu,%lu), interval=%lu-%lums",
                     reply_sid_.c_str(), static_cast<unsigned long>(tts_audio_clock_ms_),
                     static_cast<unsigned long>(cursor.index),
                     static_cast<unsigned long>(cursor.start_index),
                     static_cast<unsigned long>(cursor.end_index),
                     static_cast<unsigned long>(cursor.start_ms),
                     static_cast<unsigned long>(cursor.end_ms));
            return;
        }
    }
    ESP_LOGW(TAG, "TTS 打断时尚无未播放完的 timeline 句子: clock=%lums, received=%u",
             static_cast<unsigned long>(tts_audio_clock_ms_),
             static_cast<unsigned>(timeline_cursors_.size()));
}

bool XiaolingSessionVoice::SendFrozenReplyInterruption() {
    FrozenInterruption interruption;
    {
        std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
        if (!frozen_interruption_.valid) {
            return false;
        }
        interruption = std::move(frozen_interruption_);
        frozen_interruption_ = {};
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* params = root != nullptr ? cJSON_AddObjectToObject(root, "params") : nullptr;
    cJSON* payload = params != nullptr ? cJSON_AddObjectToObject(params, "payload") : nullptr;
    if (root == nullptr || params == nullptr || payload == nullptr) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "创建 reply_interrupted 事件失败");
        return false;
    }
    cJSON_AddStringToObject(root, "action", "event");
    cJSON_AddStringToObject(params, "event_name", "reply_interrupted");
    cJSON_AddStringToObject(payload, "sid", interruption.sid.c_str());
    cJSON_AddNumberToObject(payload, "sentence_index", interruption.cursor.index);
    cJSON_AddNumberToObject(payload, "sentence_start", interruption.cursor.start_index);
    cJSON_AddNumberToObject(payload, "sentence_end", interruption.cursor.end_index);
    const auto message = JsonToString(root);
    cJSON_Delete(root);

    const bool sent = !message.empty() && text_sender_ && text_sender_(message);
    ESP_LOGI(TAG,
             "reply_interrupted %s: sid=%s, clock=%lums, sentence=%lu, range=[%lu,%lu)",
             sent ? "已发送" : "发送失败", interruption.sid.c_str(),
             static_cast<unsigned long>(interruption.playback_position_ms),
             static_cast<unsigned long>(interruption.cursor.index),
             static_cast<unsigned long>(interruption.cursor.start_index),
             static_cast<unsigned long>(interruption.cursor.end_index));
    return sent;
}

void XiaolingSessionVoice::SendAbortSpeaking(AbortReason reason) {
    // Application 正常会在停止播放器前调用 PrepareAbortSpeaking()；这里的
    // fallback 覆盖直接调用本接口的路径，同时仍确保 event 早于 cancel。
    PrepareAbortSpeaking(reason);
    SendFrozenReplyInterruption();
    {
        std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
        pending_subtitle_url_.clear();
        tts_audio_pending_ = false;
        tts_pipeline_running_ = false;
        tts_audio_playing_ = false;
        tts_audio_clock_ms_ = 0;
    }
    CancelSubtitleStream(true);
    if (session_active_ && text_sender_ && text_sender_("{\"action\":\"cancel\"}")) {
        audio_upload_enabled_ = false;
        SetSessionActive(false);
    }
}

bool XiaolingSessionVoice::FlushPendingWakeAudio() {
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

bool XiaolingSessionVoice::CheckUplinkAudioFormat(const AudioStreamPacket& packet) {
    if (uplink_audio_format_checked_.load()) {
        return true;
    }

    if (std::strcmp(XiaolingConfig::kUplinkAudioEncoding, "raw") == 0) {
        if (packet.sample_rate != kServerSampleRate ||
            packet.payload.size() % sizeof(int16_t) != 0) {
            ESP_LOGE(TAG, "上行 PCM 校验失败: sample_rate=%d, bytes=%u",
                     packet.sample_rate, static_cast<unsigned>(packet.payload.size()));
            return false;
        }
        uplink_audio_format_checked_.store(true);
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
    uplink_audio_format_checked_.store(true);
    return true;
}

bool XiaolingSessionVoice::SendUplinkAudioPacket(const AudioStreamPacket& packet) {
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

void XiaolingSessionVoice::HandleBinary(std::vector<uint8_t> payload) {
    if (on_incoming_audio_) {
        on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
            .sample_rate = kServerSampleRate,
            .frame_duration = kServerFrameDurationMs,
            .timestamp = 0,
            .payload = std::move(payload),
        }));
    }
}

void XiaolingSessionVoice::HandleMessage(const cJSON* root) {
    const auto* action = ReadString(root, "action");
    if (action == nullptr) {
        return;
    }

    if (std::strcmp(action, "started") == 0) {
        bool preserve_playing_tts = false;
        {
            std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
            preserve_playing_tts = tts_audio_pending_ || tts_pipeline_running_ ||
                                   tts_audio_playing_;
        }
        if (preserve_playing_tts) {
            // In full duplex the previous cloud result may finish before its
            // independent HTTP TTS has drained. A fresh uplink session must
            // not erase the active timeline or interruption cursor.
            EndIat();
            iat_active_ = false;
            last_iat_text_.clear();
            ESP_LOGI(TAG, "全双工新会话已启动，保留正在播放的 TTS timeline");
        } else {
            ResetResultState();
        }
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
        bool tts_audio_active = false;
        {
            std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
            tts_audio_active = tts_audio_pending_ || tts_audio_playing_;
        }
        if (tts_audio_active) {
            // The WebSocket result lifecycle can finish while the independent
            // subtitle SSE still has buffered text to deliver. Its own done
            // event owns the text lifecycle; ending it here makes late data
            // look like a new response and rebuilds the timeline mid-playback.
            ESP_LOGI(TAG, "WS 会话结束，字幕流仍随 TTS 播放继续");
        } else {
            EndTtsText();
        }
        return;
    }
    if (std::strcmp(action, "result") != 0) {
        return;
    }

    const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const auto* nlp = cJSON_IsObject(data)
                          ? cJSON_GetObjectItemCaseSensitive(data, "nlp")
                          : nullptr;
    const char* stream_url = ReadString(nlp, "stream_url");
    const char* nlp_origin = ReadString(data, "nlp_origin");
    const char* stream_format = ReadString(nlp, "stream_format");
    const char* message_sid = ReadString(root, "sid");
    if (stream_url != nullptr && stream_url[0] != '\0' &&
        nlp_origin != nullptr && std::strcmp(nlp_origin, "reply_text") == 0) {
        if (stream_format != nullptr && std::strcmp(stream_format, "timeline") == 0) {
            SetPendingSubtitleUrl(stream_url, message_sid != nullptr ? message_sid : "");
        } else {
            ESP_LOGW(TAG, "reply_text 未声明 timeline 格式，忽略字幕流且不启用打断位置上报");
        }
    }
    const auto* sub = ReadString(data, "sub");
    if (sub == nullptr) {
        return;
    }
    if (std::strcmp(sub, "iat") == 0) {
        HandleIat(data);
    } else if (std::strcmp(sub, "tts") == 0) {
        HandleTts(data, message_sid != nullptr ? message_sid : "");
    }
}

void XiaolingSessionVoice::Reset() {
    audio_upload_enabled_ = false;
    end_requested_ = false;
    SetSessionActive(false);
    pending_wake_word_.clear();
    pending_wake_audio_.clear();
    uplink_audio_format_checked_ = false;
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kSessionStartedBit);
    }
    ResetResultState();
}

void XiaolingSessionVoice::SetSessionActive(bool active) {
    session_active_ = active;
    if (session_state_callback_) {
        session_state_callback_(active);
    }
}

void XiaolingSessionVoice::ResetResultState() {
    {
        std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
        pending_subtitle_url_.clear();
        tts_audio_pending_ = false;
        tts_pipeline_running_ = false;
        tts_audio_playing_ = false;
        tts_audio_clock_ms_ = 0;
        ResetTimelineStateLocked();
        frozen_interruption_ = {};
    }
    CancelSubtitleStream(true);
    EndIat();
    EndTtsText();
    iat_active_ = false;
    tts_text_active_ = false;
    last_iat_text_.clear();
}

void XiaolingSessionVoice::HandleIat(const cJSON* data) {
    const auto* text = ReadString(data, "text");
    if (text == nullptr) {
        return;
    }

    last_iat_text_ = text;
    auto& events = XiaolingEventBus::GetInstance();
    const bool is_last = ReadBool(data, "is_last");
    // The empty final IAT is only an end acknowledgement. Do not expose it as
    // an empty utterance to the UI or application listeners.
    if (is_last && end_requested_.load() && last_iat_text_.empty()) {
        iat_active_ = false;
        end_requested_ = false;
        audio_upload_enabled_ = false;
        SetSessionActive(false);
        return;
    }
    if (!iat_active_) {
        iat_active_ = true;
        events.Publish(XIAOLING_IAT_START, last_iat_text_);
    } else if (!ReadBool(data, "is_last")) {
        events.Publish(XIAOLING_IAT_UPDATE, last_iat_text_);
    }

    if (is_last) {
        events.Publish(XIAOLING_IAT_END, last_iat_text_);
        iat_active_ = false;
        end_requested_ = false;
    }
}

void XiaolingSessionVoice::SetPendingSubtitleUrl(const std::string& url,
                                                 const std::string& reply_sid) {
    bool start_now = false;
    {
        std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
        ResetTimelineStateLocked();
        timeline_negotiated_ = true;
        timeline_valid_ = true;
        reply_sid_ = reply_sid;
        pending_subtitle_url_ = url;
        start_now = tts_pipeline_running_ || tts_audio_playing_;
    }
    if (reply_sid.empty()) {
        ESP_LOGW(TAG, "timeline 字幕帧缺少旧回复 sid，本次可显示字幕但不做打断位置上报");
    }
    if (start_now) {
        ESP_LOGI(TAG, "TTS 音频流水线已运行，字幕 URL 到达后立即请求");
        StartPendingSubtitleStream();
    } else {
        ESP_LOGI(TAG, "timeline 字幕 URL 已暂存，等待 TTS 音频流水线运行");
    }
}

void XiaolingSessionVoice::StartTtsAudio(std::string url, const std::string& reply_sid) {
    // Invalidate an older subtitle stream immediately. The single subtitle
    // worker will observe the generation change within its bounded HTTP wait;
    // no second TLS task is created while it is unwinding.
    CancelSubtitleStream(true);
    {
        std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
        // 协议通常先下发音频 URL、再下发同 sid 的 timeline URL。如果云端
        // 顺序相反，则保留已经收到的同一回复 timeline 状态。
        const bool preserve_timeline = timeline_negotiated_ && !reply_sid.empty() &&
                                       reply_sid_ == reply_sid;
        if (!preserve_timeline) {
            ResetTimelineStateLocked();
        }
        tts_audio_pending_ = true;
        tts_pipeline_running_ = false;
        tts_audio_playing_ = false;
        tts_audio_clock_ms_ = 0;
    }
    // 音频仍拥有建连优先级。字幕 HTTPS 要等播放器进入 RUNNING，也就是
    // 音频已经建连并开始解码；但无需再等首个 PCM 真正交给 codec。
    ESP_LOGI(TAG, "TTS 音频优先启动: audio_url_bytes=%u",
             static_cast<unsigned>(url.size()));
    XiaolingEventBus::GetInstance().Publish(XIAOLING_TTS_URL, std::move(url));
}

void XiaolingSessionVoice::StartPendingSubtitleStream() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
        if (pending_subtitle_url_.empty()) {
            return;
        }
        url = std::move(pending_subtitle_url_);
        pending_subtitle_url_.clear();
    }
    StartSubtitleStream(url);
}

void XiaolingSessionVoice::OnTtsPipelinePlaying() {
    {
        std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
        tts_pipeline_running_ = true;
    }
    ESP_LOGI(TAG, "TTS 音频 HTTPS 已就绪，释放 timeline 字幕请求");
    StartPendingSubtitleStream();
}

void XiaolingSessionVoice::OnTtsOutputStarted() {
    {
        std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
        tts_audio_pending_ = false;
        tts_pipeline_running_ = true;
        tts_audio_playing_ = true;
        tts_audio_clock_ms_ = 0;
    }
    ESP_LOGI(TAG, "TTS 首个 PCM 已实际输出，timeline 播放时钟启动");
    // 兼容未收到 PLAYING 事件或 URL 更晚到达的情况。
    StartPendingSubtitleStream();
}

void XiaolingSessionVoice::OnTtsAudioProgress(uint32_t audio_clock_ms) {
    std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
    if (tts_audio_playing_) {
        tts_audio_clock_ms_ = std::max(tts_audio_clock_ms_, audio_clock_ms);
    }
}

void XiaolingSessionVoice::OnTtsPlaybackStopped(bool natural_end) {
    bool preserve_pending = false;
    {
        std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
        tts_pipeline_running_ = false;
        tts_audio_playing_ = false;
        preserve_pending = tts_audio_pending_;
        if (!preserve_pending) {
            pending_subtitle_url_.clear();
        }
        if (natural_end) {
            // 正常播完绝不补报 reply_interrupted。
            timeline_reported_ = true;
            frozen_interruption_ = {};
        }
    }
    CancelSubtitleStream(false);
    if (preserve_pending) {
        ESP_LOGI(TAG, "旧 TTS 已停止，保留新 TTS 的待启动字幕 URL");
    }
}

void XiaolingSessionVoice::HandleTts(const cJSON* data, const std::string& reply_sid) {
    if (const auto* content = ReadString(data, "content")) {
        std::string url;
        if (content != nullptr && Base64Codec::Decode(content, url) && !url.empty() &&
            std::memchr(url.data(), '\0', url.size()) == nullptr) {
            StartTtsAudio(std::move(url), reply_sid);
        } else {
            ESP_LOGE(TAG, "小聆 TTS URL 解码失败");
        }
    }
}

void XiaolingSessionVoice::StartSubtitleStream(const std::string& url) {
    // StartTtsAudio() already interrupted and cleared the previous response.
    // Publishing another interrupt here would discard segments that arrived
    // during the audio prebuffer window before the UI audio clock starts.
    CancelSubtitleStream(false);
    if (subtitle_fetcher_ != nullptr) {
        subtitle_fetcher_->Start(url);
    }
}

void XiaolingSessionVoice::CancelSubtitleStream(bool notify_interrupt) {
    if (subtitle_fetcher_ != nullptr) {
        subtitle_fetcher_->Cancel();
    }
    tts_text_active_.store(false);
    if (notify_interrupt) {
        XiaolingEventBus::GetInstance().Publish(XIAOLING_TTS_TXT_INTERRUPT);
    }
}

void XiaolingSessionVoice::ResetTimelineStateLocked() {
    timeline_negotiated_ = false;
    timeline_valid_ = false;
    timeline_reported_ = false;
    reply_sid_.clear();
    timeline_cursors_.clear();
    expected_timeline_index_ = 0;
    expected_start_index_ = 0;
    previous_timeline_end_ms_ = 0;
}

void XiaolingSessionVoice::HandleSubtitleEvent(int event, const std::string& text) {
    auto& events = XiaolingEventBus::GetInstance();
    if (event == 0) {
        cJSON* root = cJSON_ParseWithLength(text.data(), text.size());
        const cJSON* item = root;
        if (cJSON_IsObject(root)) {
            const auto* nested = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsObject(nested)) {
                item = nested;
            }
        }
        const auto* subtitle = cJSON_IsObject(item)
                                   ? cJSON_GetObjectItemCaseSensitive(item, "text")
                                   : nullptr;
        uint32_t parsed_index = 0;
        uint32_t start_index = 0;
        uint32_t end_index = 0;
        uint32_t start_ms = 0;
        uint32_t end_ms = 0;
        uint32_t codepoints = 0;
        const bool fields_valid = cJSON_IsString(subtitle) &&
            subtitle->valuestring != nullptr && subtitle->valuestring[0] != '\0' &&
            ReadUint32(item, "index", &parsed_index) &&
            ReadUint32(item, "start_index", &start_index) &&
            ReadUint32(item, "end_index", &end_index) &&
            ReadUint32(item, "start_ms", &start_ms) &&
            ReadUint32(item, "end_ms", &end_ms) &&
            CountUtf8Codepoints(subtitle->valuestring, &codepoints);
        bool valid = false;
        uint32_t expected_index = 0;
        uint32_t expected_start = 0;
        uint32_t previous_end = 0;
        size_t cursor_count = 0;
        {
            std::lock_guard<std::mutex> lock(subtitle_state_mutex_);
            expected_index = expected_timeline_index_;
            expected_start = expected_start_index_;
            previous_end = previous_timeline_end_ms_;
            cursor_count = timeline_cursors_.size();
            valid = timeline_negotiated_ && timeline_valid_ && fields_valid &&
                parsed_index == expected_timeline_index_ &&
                start_index == expected_start_index_ && end_index > start_index &&
                end_index - start_index == codepoints && end_ms > start_ms &&
                start_ms >= previous_timeline_end_ms_ &&
                timeline_cursors_.size() < kMaxTimelineSegments;
            if (valid) {
                timeline_cursors_.push_back({parsed_index, start_index, end_index,
                                             start_ms, end_ms});
                expected_timeline_index_ = parsed_index + 1;
                expected_start_index_ = end_index;
                previous_timeline_end_ms_ = end_ms;
            } else if (timeline_negotiated_) {
                // 任一条 timeline 非法后，本次回复永久降级为“只播放、不上报”。
                timeline_valid_ = false;
            }
        }
        cJSON_Delete(root);
        if (!valid) {
            ESP_LOGW(TAG,
                     "timeline 校验失败，停用本次打断位置上报: fields=%d, index=%lu/%lu, range=[%lu,%lu)/start=%lu, codepoints=%lu, time=%lu-%lu/prev=%lu, cursors=%u",
                     fields_valid ? 1 : 0,
                     static_cast<unsigned long>(parsed_index),
                     static_cast<unsigned long>(expected_index),
                     static_cast<unsigned long>(start_index),
                     static_cast<unsigned long>(end_index),
                     static_cast<unsigned long>(expected_start),
                     static_cast<unsigned long>(codepoints),
                     static_cast<unsigned long>(start_ms),
                     static_cast<unsigned long>(end_ms),
                     static_cast<unsigned long>(previous_end),
                     static_cast<unsigned>(cursor_count));
            return;
        }
        ESP_LOGI(TAG,
                 "timeline 游标已保存: index=%lu, range=[%lu,%lu), interval=%lu-%lums",
                 static_cast<unsigned long>(parsed_index),
                 static_cast<unsigned long>(start_index),
                 static_cast<unsigned long>(end_index),
                 static_cast<unsigned long>(start_ms),
                 static_cast<unsigned long>(end_ms));
        if (!tts_text_active_.exchange(true)) {
            events.Publish(XIAOLING_TTS_TXT_START);
        }
        events.Publish(XIAOLING_TTS_TXT_UPDATE, text);
    } else if (event == 1) {
        if (tts_text_active_.exchange(false)) {
            events.Publish(XIAOLING_TTS_TXT_END);
        }
    }
}

void XiaolingSessionVoice::EndIat() {
    if (!iat_active_) {
        return;
    }
    XiaolingEventBus::GetInstance().Publish(XIAOLING_IAT_END, last_iat_text_);
    iat_active_ = false;
}

void XiaolingSessionVoice::EndTtsText() {
    if (tts_text_active_.exchange(false)) {
        XiaolingEventBus::GetInstance().Publish(XIAOLING_TTS_TXT_END);
    }
}
