#include "xiaoling_conn.h"

#include "board.h"
#include "system/system_info.h"
#include "xiaoling/xiaoling_events.h"
#include "xiaoling/session/session_voice.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <utility>

#define TAG "XiaolingConn"

XiaolingConn::XiaolingConn() {
    event_group_ = xEventGroupCreate();
    voice_ = std::make_unique<XiaolingSessionVoice>();
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
        [this](XiaolingSessionVoiceError error) {
            SetError(error == XiaolingSessionVoiceError::kServerTimeout
                         ? XiaolingError::ServerTimeout
                         : XiaolingError::ServerError);
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
        SetError(XiaolingError::ServerNotConnected);
        return false;
    }

    // WebSocket 的建立交给底层网络组件；本层只建立一次并启动心跳观测任务。
    connecting_ = true;
    const bool connected = ConnectOnce();
    connecting_ = false;
    if (!connected) {
        SetError(XiaolingError::ServerNotConnected);
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

void XiaolingConn::OnNetworkError(std::function<void(XiaolingError)> callback) {
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
    const bool was_transport_connected = transport_connected_.exchange(false);
    session_active_ = false;
    if (was_transport_connected || protocol_connected_.exchange(false)) {
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
        if (desired_connected_ && !transport_connected_ && !connecting_) {
            if (reconnect_attempts_ < 3) {
                ++reconnect_attempts_;
                ESP_LOGW(TAG, "小聆 WebSocket 断开，开始第 %lu 次重连",
                         static_cast<unsigned long>(reconnect_attempts_));
                if (ConnectOnce()) {
                    reconnect_attempts_ = 0;
                    disconnected_event_published_ = false;
                }
            } else if (!disconnected_event_published_) {
                PublishDisconnected();
                disconnected_event_published_ = true;
            }
        } else if (desired_connected_ && transport_connected_) {
            reconnect_attempts_ = 0;
            disconnected_event_published_ = false;
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

        const auto wait_ms = transport_connected_ ? 250 : kReconnectIntervalMs;
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
    const bool was_transport_connected = transport_connected_.exchange(false);
    session_active_ = false;
    if (voice_ != nullptr) {
        voice_->Reset();
    }
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kProtocolConnectedBit);
    }
    if (was_transport_connected || protocol_connected_.exchange(false)) {
        ESP_LOGI(TAG, "小聆连接已断开，等待自动重连");
    }
    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
}

void XiaolingConn::PublishDisconnected() {
    ESP_LOGE(TAG, "小聆 WebSocket 连续 3 次重连失败");
    XiaolingEventBus::GetInstance().Publish(XIAOLING_DISCONNECTED);
    if (on_disconnected_) {
        on_disconnected_();
    }
}

void XiaolingConn::SetError(XiaolingError error) {
    error_occurred_ = true;
    if (on_network_error_) {
        on_network_error_(error);
    }
}
