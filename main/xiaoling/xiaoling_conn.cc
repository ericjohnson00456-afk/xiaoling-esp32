#include "xiaoling_conn.h"

#include "i18n/lang_config.h"
#include "board.h"
#include "system_info.h"
#include "xiaoling_events.h"
#include "xiaoling_voice.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <utility>

#define TAG "XiaolingConn"

XiaolingConn::XiaolingConn() {
    event_group_ = xEventGroupCreate();
    voice_ = std::make_unique<XiaolingVoice>();
    voice_->SetTransport(
        [this](const std::string& text) {
            return SendText(text);
        },
        [this](const uint8_t* data, size_t size) {
            return SendBinary(data, size);
        },
        [this](bool active) {
            SetSessionActive(active);
        },
        [this](XiaolingVoiceError error) {
            SetError(error == XiaolingVoiceError::kServerTimeout
                         ? Lang::Strings::SERVER_TIMEOUT
                         : Lang::Strings::SERVER_ERROR);
        });
}

XiaolingConn::~XiaolingConn() {
    Disconnect();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

bool XiaolingConn::Start() {
    return Connect();
}

bool XiaolingConn::Connect() {
    if (IsConnected()) {
        return true;
    }
    if (!EnsureAuthenticated() || event_group_ == nullptr) {
        return false;
    }

    Disconnect();
    error_occurred_ = false;
    protocol_connected_ = false;
    xEventGroupClearBits(event_group_, kProtocolConnectedBit);
    if (!StartSupervisor()) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    const auto bits = xEventGroupWaitBits(event_group_, kProtocolConnectedBit,
                                          pdTRUE, pdFALSE,
                                          pdMS_TO_TICKS(kProtocolConnectTimeoutMs));
    if (!(bits & kProtocolConnectedBit) || !protocol_connected_) {
        ESP_LOGE(TAG, "等待小聆 connected 消息超时");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        StopSupervisor();
        return false;
    }
    return true;
}

void XiaolingConn::Disconnect() {
    if (voice_ != nullptr) {
        voice_->Reset();
    }
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kProtocolConnectedBit);
    }
    StopSupervisor();
}

bool XiaolingConn::IsConnected() const {
    return transport_connected_ && protocol_connected_ && !error_occurred_;
}

bool XiaolingConn::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    return voice_ != nullptr && voice_->SendAudio(std::move(packet));
}

void XiaolingConn::SendWakeWordDetected(const std::string& wake_word) {
    if (voice_ != nullptr) {
        voice_->SendWakeWordDetected(wake_word);
    }
}

void XiaolingConn::SendStartListening(ListeningMode mode) {
    if (voice_ != nullptr) {
        voice_->SendStartListening(mode);
    }
}

void XiaolingConn::SendStopListening() {
    if (voice_ != nullptr) {
        voice_->SendStopListening();
    }
}

void XiaolingConn::SendAbortSpeaking(AbortReason reason) {
    if (voice_ != nullptr) {
        voice_->SendAbortSpeaking(reason);
    }
}

void XiaolingConn::SetNarrowbandMode(bool enabled) {
    if (voice_ != nullptr) {
        voice_->SetNarrowbandMode(enabled);
    }
}

int XiaolingConn::server_sample_rate() const {
    return voice_ != nullptr ? voice_->server_sample_rate() : 16000;
}

int XiaolingConn::server_frame_duration() const {
    return voice_ != nullptr ? voice_->server_frame_duration() : 60;
}

const std::string& XiaolingConn::session_id() const {
    static const std::string empty_session_id;
    return voice_ != nullptr ? voice_->session_id() : empty_session_id;
}

void XiaolingConn::OnIncomingAudio(
        std::function<void(std::unique_ptr<AudioStreamPacket>)> callback) {
    if (voice_ != nullptr) {
        voice_->OnIncomingAudio(std::move(callback));
    }
}

void XiaolingConn::OnIncomingJson(std::function<void(const cJSON*)> callback) {
    on_incoming_json_ = std::move(callback);
}

void XiaolingConn::OnNetworkError(std::function<void(const std::string&)> callback) {
    on_network_error_ = std::move(callback);
}

void XiaolingConn::OnConnected(std::function<void()> callback) {
    on_connected_ = std::move(callback);
}

void XiaolingConn::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = std::move(callback);
}

bool XiaolingConn::StartSupervisor() {
    desired_connected_ = true;
    if (!running_.exchange(true)) {
        if (xTaskCreate(TaskEntry, "xiaoling_conn", 6144, this, 4, &task_handle_) != pdPASS) {
            ESP_LOGE(TAG, "创建小聆连接管理任务失败");
            task_handle_ = nullptr;
            running_ = false;
            desired_connected_ = false;
            return false;
        }
    } else if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
    return true;
}

void XiaolingConn::StopSupervisor() {
    desired_connected_ = false;
    running_ = false;
    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }

    std::unique_ptr<CloudWebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        websocket = std::move(websocket_);
    }
    if (websocket != nullptr) {
        websocket->Disconnect();
    }
    transport_connected_ = false;
    session_active_ = false;
    if (protocol_connected_.exchange(false)) {
        XiaolingEventBus::GetInstance().Publish(XIAOLING_DISCONNECTED);
        if (on_disconnected_) {
            on_disconnected_();
        }
    }

    // 等待正在进行的网络握手返回，避免连接任务的生命周期超过当前对象。
    for (int i = 0; i < 1200 && task_handle_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool XiaolingConn::SendBinary(const uint8_t* data, size_t size) {
    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        sent = websocket_ != nullptr && websocket_->SendBinary(data, size);
    }
    if (!sent) {
        HandleTransportDisconnected(generation_.load());
    }
    return sent;
}

void XiaolingConn::SetSessionActive(bool active) {
    session_active_ = active;
}

void XiaolingConn::TaskEntry(void* arg) {
    static_cast<XiaolingConn*>(arg)->Run();
}

void XiaolingConn::Run() {
    last_heartbeat_us_ = esp_timer_get_time();
    while (running_) {
        if (desired_connected_ && !transport_connected_) {
            ConnectOnce();
        } else if (desired_connected_ && transport_connected_ && !session_active_) {
            const int64_t now = esp_timer_get_time();
            if (now - last_heartbeat_us_ >= static_cast<int64_t>(kHeartbeatIntervalMs) * 1000) {
                bool sent = false;
                {
                    std::lock_guard<std::mutex> lock(websocket_mutex_);
                    sent = websocket_ != nullptr && websocket_->Ping();
                }
                if (!sent) {
                    HandleTransportDisconnected(generation_.load());
                }
                last_heartbeat_us_ = now;
            }
        }

        const auto wait_ms = transport_connected_ ? kHeartbeatIntervalMs : kReconnectIntervalMs;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
    task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

bool XiaolingConn::ConnectOnce() {
    if (!EnsureAuthenticated()) {
        ESP_LOGW(TAG, "小聆鉴权信息不可用，稍后重试连接");
        return false;
    }

    const auto url = BuildWebSocketUrl();
    const auto access_token = LoadAccessToken();
    if (url.empty() || access_token.empty()) {
        return false;
    }
    const CloudWebSocketHeaders headers = {
        {"Authorization", NormalizeBearerToken(access_token)},
        {"Device-Id", SystemInfo::GetCloudDeviceId()},
        {"Client-Id", Board::GetInstance().GetUuid()},
    };

    const uint32_t generation = generation_.fetch_add(1) + 1;
    auto websocket = std::make_unique<CloudWebSocket>(1);
    websocket->OnText([this, generation](std::string text) {
        if (generation == generation_.load()) {
            HandleIncomingText(text);
        }
    });
    websocket->OnBinary([this, generation](std::vector<uint8_t> payload) {
        if (generation == generation_.load() && voice_ != nullptr) {
            voice_->HandleBinary(std::move(payload));
        }
    });
    websocket->OnConnected([this, generation]() {
        if (generation != generation_.load()) {
            return;
        }
        transport_connected_ = true;
        last_heartbeat_us_ = esp_timer_get_time();
        ESP_LOGI(TAG, "小聆 WebSocket 传输层已连接");
    });
    websocket->OnDisconnected([this, generation]() {
        HandleTransportDisconnected(generation);
    });
    websocket->OnError([generation](int error) {
        ESP_LOGE(TAG, "小聆 WebSocket 错误: generation=%lu, error=%d",
                 static_cast<unsigned long>(generation), error);
    });

    std::unique_ptr<CloudWebSocket> previous;
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        previous = std::move(websocket_);
    }
    if (previous != nullptr) {
        previous->Disconnect();
    }

    ESP_LOGI(TAG, "正在连接小聆 WebSocket");
    bool connected = false;
    {
        // 在握手前先挂到成员上；connected 回调触发后，语音模块可立即发送 start。
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        websocket_ = std::move(websocket);
        connected = websocket_->Connect(url, headers);
    }
    if (!connected) {
        transport_connected_ = false;
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        websocket_.reset();
        return false;
    }
    if (!running_ || !desired_connected_ || generation != generation_.load()) {
        std::unique_ptr<CloudWebSocket> stale_websocket;
        {
            std::lock_guard<std::mutex> lock(websocket_mutex_);
            stale_websocket = std::move(websocket_);
        }
        if (stale_websocket != nullptr) {
            stale_websocket->Disconnect();
        }
        return false;
    }
    return true;
}

void XiaolingConn::HandleTransportDisconnected(uint32_t generation) {
    if (generation != generation_.load()) {
        return;
    }
    transport_connected_ = false;
    session_active_ = false;
    if (voice_ != nullptr) {
        voice_->Reset();
    }
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kProtocolConnectedBit);
    }
    if (protocol_connected_.exchange(false)) {
        ESP_LOGI(TAG, "小聆连接已断开");
        XiaolingEventBus::GetInstance().Publish(XIAOLING_DISCONNECTED);
        if (on_disconnected_) {
            on_disconnected_();
        }
    }
    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
}

void XiaolingConn::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_) {
        on_network_error_(message);
    }
}
