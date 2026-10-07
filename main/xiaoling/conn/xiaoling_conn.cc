#include "xiaoling_conn.h"

#include "board.h"
#include "system/system_info.h"
#include "event/app_event.h"
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

    // Serialize an explicit connect with the supervisor's reconnect attempt.
    // StartSupervisor() creates the task immediately, so setting this guard
    // after starting it allows the task to race ConnectOnce() and create two
    // WebSocket transports for the same connection object.
    bool was_connecting = false;
    if (!connecting_.compare_exchange_strong(was_connecting, true)) {
        ESP_LOGW(TAG, "小聆 WebSocket 正在连接，跳过重复连接请求");
        return false;
    }

    Disconnect();
    error_occurred_ = false;
    protocol_connected_ = false;
    xEventGroupClearBits(event_group_, kProtocolConnectedBit);
    if (!StartSupervisor()) {
        connecting_ = false;
        SetError(XiaolingError::ServerNotConnected);
        return false;
    }

    // WebSocket 的建立交给底层网络组件；本层只建立一次并启动心跳观测任务。
    const bool connected = ConnectOnce();
    connecting_ = false;
    if (!connected) {
        SetError(XiaolingError::ServerNotConnected);
        return false;
    }
    return true;
}

void XiaolingConn::Disconnect() {
    ClearUplinkQueue();
    ClearControlQueue();
    FailPendingAlarmTtsRequest();
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

bool XiaolingConn::IsSessionActive() const {
    return session_active_.load();
}

bool XiaolingConn::CanUploadAudio() const {
    return voice_ != nullptr && voice_->audio_upload_enabled();
}

XiaolingUplinkDiagnostics XiaolingConn::GetUplinkDiagnostics() {
    XiaolingUplinkDiagnostics diagnostics;
    {
        std::lock_guard<std::mutex> lock(uplink_mutex_);
        diagnostics.queue = uplink_queue_.size();
        diagnostics.dropped_packets = dropped_uplink_packets_;
    }
    diagnostics.connected = IsConnected();
    diagnostics.session_active = session_active_.load();
    diagnostics.upload_enabled = CanUploadAudio();
    diagnostics.enqueued_packets = uplink_enqueued_packets_.load();
    diagnostics.rejected_packets = uplink_rejected_packets_.load();
    diagnostics.batches_started = uplink_batches_started_.load();
    diagnostics.batches_completed = uplink_batches_completed_.load();
    diagnostics.batches_failed = uplink_batches_failed_.load();
    diagnostics.sent_frames = uplink_sent_frames_.load();
    diagnostics.active_sequence = uplink_active_sequence_.load();
    const int64_t send_started_us = uplink_send_started_us_.load();
    diagnostics.send_in_progress = send_started_us != 0;
    if (send_started_us != 0) {
        diagnostics.active_send_ms = (esp_timer_get_time() - send_started_us) / 1000;
    }
    const int64_t last_completed_us = uplink_last_send_completed_us_.load();
    if (last_completed_us != 0) {
        diagnostics.last_send_completed_ms =
            (esp_timer_get_time() - last_completed_us) / 1000;
    }
    return diagnostics;
}

bool XiaolingConn::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (packet == nullptr || packet->payload.empty() || !IsConnected()) {
        const uint32_t rejected = uplink_rejected_packets_.fetch_add(1) + 1;
        if ((rejected % 25) == 1) {
            ESP_LOGW(TAG,
                     "上行链路[连接拒绝]: count=%lu, packet=%d, bytes=%u, "
                     "connected=%d, transport=%d, protocol=%d, error=%d",
                     static_cast<unsigned long>(rejected), packet != nullptr ? 1 : 0,
                     packet != nullptr ? static_cast<unsigned>(packet->payload.size()) : 0,
                     IsConnected() ? 1 : 0, transport_connected_.load() ? 1 : 0,
                     protocol_connected_.load() ? 1 : 0, error_occurred_ ? 1 : 0);
        }
        return false;
    }

    bool wake_sender = false;
    {
        std::lock_guard<std::mutex> lock(uplink_mutex_);
        wake_sender = uplink_queue_.empty();
        if (uplink_queue_.size() >= kMaxUplinkPackets) {
            // Uplink audio is useful only in real time. Preserve the newest
            // post-AEC speech instead of building a stale network backlog.
            uplink_queue_.pop_front();
            if ((++dropped_uplink_packets_ % 25) == 1) {
                ESP_LOGW(TAG, "上行网络队列满，丢弃旧包: count=%lu",
                         static_cast<unsigned long>(dropped_uplink_packets_));
            }
        }
        uplink_queue_.push_back(std::move(packet));
        uplink_enqueued_packets_.fetch_add(1);
    }
    if (wake_sender && task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
    return true;
}

void XiaolingConn::OnAudioSent(std::function<void(uint32_t)> callback) {
    on_audio_sent_ = std::move(callback);
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

bool XiaolingConn::StopListeningForTts() {
    return voice_ != nullptr && voice_->StopListeningForTts();
}

bool XiaolingConn::EndSession() {
    return voice_ != nullptr && voice_->EndSession();
}

bool XiaolingConn::CancelSession() {
    return voice_ != nullptr && voice_->CancelSession();
}

void XiaolingConn::PrepareAbortSpeaking(AbortReason reason) {
    if (voice_ != nullptr) {
        voice_->PrepareAbortSpeaking(reason);
    }
}

void XiaolingConn::SendAbortSpeaking(AbortReason reason) {
    if (voice_ != nullptr) {
        voice_->SendAbortSpeaking(reason);
    }
}

void XiaolingConn::HandleIatBargeIn() {
    if (voice_ != nullptr) {
        voice_->HandleIatBargeIn();
    }
}

void XiaolingConn::PrepareTtsReplay() {
    if (voice_ != nullptr) {
        voice_->PrepareTtsReplay();
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
        // Uplink audio and WebSocket control messages share the highest
        // application priority with the TTS HTTP pull task.
        if (xTaskCreate(TaskEntry, "xiaoling_conn", 6144, this,
                        kUplinkTaskPriority, &task_handle_) != pdPASS) {
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

    std::unique_ptr<XiaolingConnWebSocket> websocket;
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
        AppEvent::Publish(AppEvent::Id::XiaolingDisconnected);
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
    const uint32_t sequence = uplink_active_sequence_.load();
    const bool trace_send = sequence <= 3 || (sequence % 50) == 1;
    const int64_t lock_started_us = esp_timer_get_time();
    if (trace_send) {
        ESP_LOGI(TAG, "上行链路[WS锁等待]: seq=%lu, bytes=%u",
                 static_cast<unsigned long>(sequence), static_cast<unsigned>(size));
    }
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        const uint32_t lock_wait_ms = static_cast<uint32_t>(
            (esp_timer_get_time() - lock_started_us) / 1000);
        if (lock_wait_ms >= 80) {
            ESP_LOGW(TAG, "上行链路[WS锁获得]: seq=%lu, wait=%lums, socket=%d",
                     static_cast<unsigned long>(sequence),
                     static_cast<unsigned long>(lock_wait_ms), websocket_ != nullptr ? 1 : 0);
        } else if (trace_send) {
            ESP_LOGI(TAG, "上行链路[WS锁获得]: seq=%lu, wait=%lums, socket=%d",
                     static_cast<unsigned long>(sequence),
                     static_cast<unsigned long>(lock_wait_ms), websocket_ != nullptr ? 1 : 0);
        }
        sent = websocket_ != nullptr && websocket_->SendBinary(data, size);
    }
    if (!sent) {
        HandleTransportDisconnected(generation_.load());
    }
    return sent;
}

void XiaolingConn::SetSessionActive(bool active) {
    session_active_ = active;
    if (!active) {
        ClearUplinkQueue();
    }
}

void XiaolingConn::ClearUplinkQueue() {
    std::lock_guard<std::mutex> lock(uplink_mutex_);
    uplink_queue_.clear();
}

void XiaolingConn::ClearControlQueue() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    control_queue_.clear();
}

bool XiaolingConn::SendPendingControl() {
    ControlFrame frame;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (control_queue_.empty()) {
            return false;
        }
        frame = std::move(control_queue_.front());
        control_queue_.pop_front();
    }

    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        sent = websocket_ != nullptr &&
               (frame.binary
                    ? websocket_->SendBinary(
                          reinterpret_cast<const uint8_t*>(frame.payload.data()),
                          frame.payload.size())
                    : websocket_->SendText(frame.payload));
    }
    if (!sent) {
        ESP_LOGW(TAG, "WebSocket %s控制消息发送失败，触发链路重连",
                 frame.binary ? "二进制" : "文本");
        HandleTransportDisconnected(generation_.load());
    }
    return true;
}

bool XiaolingConn::SendPendingAudio() {
    size_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(uplink_mutex_);
        pending = uplink_queue_.size();
    }
    if (pending == 0) {
        return false;
    }

    // Accumulate only a short window of 20 ms packets. The length-prefixed
    // opus-wb frames can share one WebSocket message, greatly reducing TLS
    // small-write pressure while keeping barge-in latency bounded.
    if (pending < kUplinkBatchPackets) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kUplinkBatchWaitMs));
    }

    std::vector<std::unique_ptr<AudioStreamPacket>> packets;
    packets.reserve(kUplinkBatchPackets);
    uint32_t duration_ms = 0;
    size_t queued_after_pop = 0;
    {
        std::lock_guard<std::mutex> lock(uplink_mutex_);
        while (!uplink_queue_.empty() && packets.size() < kUplinkBatchPackets) {
            const auto frame_duration = uplink_queue_.front()->frame_duration;
            if (frame_duration > 0) {
                duration_ms += static_cast<uint32_t>(frame_duration);
            }
            packets.push_back(std::move(uplink_queue_.front()));
            uplink_queue_.pop_front();
        }
        queued_after_pop = uplink_queue_.size();
    }

    if (!running_ || !desired_connected_ || packets.empty()) {
        return true;
    }
    const size_t packet_count = packets.size();
    const int64_t send_started_us = esp_timer_get_time();
    const uint32_t sequence = uplink_batches_started_.fetch_add(1) + 1;
    uplink_active_sequence_.store(sequence);
    uplink_send_started_us_.store(send_started_us);
    const bool trace_batch = sequence <= 3 || (sequence % 50) == 1;
    if (trace_batch) {
        ESP_LOGI(TAG,
                 "上行链路[WS开始]: seq=%lu, frames=%u, audio=%lums, queue_after=%u, "
                 "connected=%d, session=%d, upload=%d, task_prio=%u",
                 static_cast<unsigned long>(sequence),
                 static_cast<unsigned>(packet_count),
                 static_cast<unsigned long>(duration_ms),
                 static_cast<unsigned>(queued_after_pop),
                 IsConnected() ? 1 : 0, session_active_.load() ? 1 : 0,
                 CanUploadAudio() ? 1 : 0,
                 static_cast<unsigned>(uxTaskPriorityGet(nullptr)));
    }
    const bool sent = voice_ != nullptr && voice_->SendAudioBatch(std::move(packets));
    const uint32_t send_elapsed_ms = static_cast<uint32_t>(
        (esp_timer_get_time() - send_started_us) / 1000);
    uplink_send_started_us_.store(0);
    uplink_last_send_completed_us_.store(esp_timer_get_time());
    uplink_batches_completed_.fetch_add(1);
    if (sent) {
        uplink_sent_frames_.fetch_add(packet_count);
    } else {
        uplink_batches_failed_.fetch_add(1);
    }
    if (send_elapsed_ms >= 80 || !sent) {
        size_t queued = 0;
        {
            std::lock_guard<std::mutex> lock(uplink_mutex_);
            queued = uplink_queue_.size();
        }
        ESP_LOGW(TAG,
                 "上行链路[WS结束]: seq=%lu, frames=%u, audio=%lums, wait=%lums, "
                 "queued=%u, ok=%d, completed=%lu, failed=%lu",
                 static_cast<unsigned long>(sequence),
                 static_cast<unsigned>(packet_count),
                 static_cast<unsigned long>(duration_ms),
                 static_cast<unsigned long>(send_elapsed_ms),
                 static_cast<unsigned>(queued), sent ? 1 : 0,
                 static_cast<unsigned long>(uplink_batches_completed_.load()),
                 static_cast<unsigned long>(uplink_batches_failed_.load()));
    } else if (trace_batch) {
        ESP_LOGI(TAG,
                 "上行链路[WS结束]: seq=%lu, frames=%u, audio=%lums, wait=%lums, "
                 "queued=%u, ok=%d",
                 static_cast<unsigned long>(sequence),
                 static_cast<unsigned>(packet_count),
                 static_cast<unsigned long>(duration_ms),
                 static_cast<unsigned long>(send_elapsed_ms),
                 static_cast<unsigned>(queued_after_pop), sent ? 1 : 0);
    }
    if (sent && on_audio_sent_) {
        on_audio_sent_(duration_ms);
    }
    return true;
}

void XiaolingConn::TaskEntry(void* arg) {
    static_cast<XiaolingConn*>(arg)->Run();
}

void XiaolingConn::Run() {
    last_heartbeat_us_ = esp_timer_get_time();
    while (running_) {
        if (desired_connected_ && !transport_connected_ && !connecting_) {
            if (reconnect_attempts_ < 3) {
                bool was_connecting = false;
                if (!connecting_.compare_exchange_strong(was_connecting, true)) {
                    continue;
                }
                ++reconnect_attempts_;
                ESP_LOGW(TAG, "小聆 WebSocket 断开，开始第 %lu 次重连",
                         static_cast<unsigned long>(reconnect_attempts_));
                const bool connected = ConnectOnce();
                connecting_ = false;
                if (connected) {
                    reconnect_attempts_ = 0;
                    disconnected_event_published_ = false;
                }
            } else if (!disconnected_event_published_) {
                PublishDisconnected();
                disconnected_event_published_ = true;
            }
        } else if (desired_connected_ && transport_connected_) {
            disconnected_event_published_ = false;
            if (error_occurred_ && protocol_connected_) {
                // 传输层和协议层都在，但错误标志被闩住（如会话超时后不会再收到
                // 新的 connected），IsConnected() 恒为 false，主动断开走重连自愈。
                ESP_LOGW(TAG, "小聆连接带错误标志运行，主动断开重连");
                ForceReconnect();
            } else {
                reconnect_attempts_ = 0;
            }
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

        if (desired_connected_ && transport_connected_ && protocol_connected_ &&
            SendPendingControl()) {
            // Control frames (start/end/interruption/MCP) are sparse and must
            // be observed before another batch of realtime audio.
            continue;
        }

        if (desired_connected_ && transport_connected_ && protocol_connected_ &&
            SendPendingAudio()) {
            // Drain a backlog in bounded batches before sleeping again.
            continue;
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
    if (voice_ != nullptr) {
        voice_->SetAccessToken(access_token);
    }
    const XiaolingWsHeaders headers = {
        {"Authorization", NormalizeBearerToken(access_token)},
        {"Device-Id", SystemInfo::GetCloudDeviceId()},
        {"Client-Id", Board::GetInstance().GetUuid()},
    };

    const uint32_t generation = generation_.fetch_add(1) + 1;
    auto websocket = std::make_unique<XiaolingConnWebSocket>(1);
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

    std::unique_ptr<XiaolingConnWebSocket> previous;
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
        std::unique_ptr<XiaolingConnWebSocket> stale_websocket;
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
    ClearUplinkQueue();
    ClearControlQueue();
    FailPendingAlarmTtsRequest();
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

void XiaolingConn::ForceReconnect() {
    // 复用代际回调路径：断开当前 WebSocket 会触发 OnDisconnected ->
    // HandleTransportDisconnected，错误标志在重连握手完成前保持闩住，
    // 直到服务器下发新的 connected 才清除。
    std::unique_ptr<XiaolingConnWebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        websocket = std::move(websocket_);
    }
    if (websocket != nullptr) {
        websocket->Disconnect();
    } else {
        transport_connected_ = false;
        if (task_handle_ != nullptr) {
            xTaskNotifyGive(task_handle_);
        }
    }
}

void XiaolingConn::PublishDisconnected() {
    ESP_LOGE(TAG, "小聆 WebSocket 连续 3 次重连失败");
    AppEvent::Publish(AppEvent::Id::XiaolingDisconnected);
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
