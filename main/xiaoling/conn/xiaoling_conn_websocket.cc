#include "xiaoling_conn_websocket.h"

#include "network_diagnostics.h"
#include "realtime_transport_scope.h"

#include "board.h"
#include "web_socket.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <utility>

#define TAG "XiaolingWs"

namespace {
constexpr uint32_t kControlSendTimeoutMs = 250;
constexpr uint32_t kBinarySendTimeoutMs = 2000;

void LogBinarySendDiagnostics(uint32_t sequence, size_t size,
                              int64_t started_us) {
    const uint32_t elapsed_ms = static_cast<uint32_t>(
        (esp_timer_get_time() - started_us) / 1000);
    const unsigned stack_free_words = static_cast<unsigned>(
        uxTaskGetStackHighWaterMark(nullptr));

    wifi_ap_record_t ap_info = {};
    const esp_err_t wifi_result = esp_wifi_sta_get_ap_info(&ap_info);
    if (wifi_result == ESP_OK) {
        ESP_LOGW(TAG,
                 "WS TX diagnostics: seq=%lu, bytes=%u, elapsed=%lums, "
                 "wifi_rssi=%d dBm, channel=%u, second_channel=%d, "
                 "free_heap=%lu, min_free_heap=%lu, task_stack_free=%u words",
                 static_cast<unsigned long>(sequence),
                 static_cast<unsigned>(size),
                 static_cast<unsigned long>(elapsed_ms),
                 static_cast<int>(ap_info.rssi),
                 static_cast<unsigned>(ap_info.primary),
                 static_cast<int>(ap_info.second),
                 static_cast<unsigned long>(esp_get_free_heap_size()),
                 static_cast<unsigned long>(esp_get_minimum_free_heap_size()),
                 stack_free_words);
        return;
    }

    ESP_LOGW(TAG,
             "WS TX diagnostics: seq=%lu, bytes=%u, elapsed=%lums, "
             "wifi_info_error=%s, free_heap=%lu, min_free_heap=%lu, "
             "task_stack_free=%u words",
             static_cast<unsigned long>(sequence),
             static_cast<unsigned>(size),
             static_cast<unsigned long>(elapsed_ms),
             esp_err_to_name(wifi_result),
             static_cast<unsigned long>(esp_get_free_heap_size()),
             static_cast<unsigned long>(esp_get_minimum_free_heap_size()),
             stack_free_words);
}
}  // namespace

XiaolingConnWebSocket::XiaolingConnWebSocket(int connect_id)
    : connect_id_(connect_id) {
}

XiaolingConnWebSocket::~XiaolingConnWebSocket() {
    Disconnect();
}

bool XiaolingConnWebSocket::Connect(const std::string& url, const XiaolingWsHeaders& headers) {
    Disconnect();
    rx_binary_sequence_ = 0;
    tx_binary_sequence_ = 0;
    endpoint_url_ = url;
    NetworkDiagnostics::Initialize();

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network interface is unavailable");
        return false;
    }

    websocket_ = network->CreateWebSocket(connect_id_);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create managed WebSocket");
        return false;
    }

    for (const auto& [name, value] : headers) {
        websocket_->SetHeader(name.c_str(), value.c_str());
        // 请求头只记录名称和长度，避免身份凭据写入串口日志。
        ESP_LOGI(TAG, "Applied WebSocket header: %s (value hidden, bytes=%u)",
                 name.c_str(), static_cast<unsigned>(value.size()));
    }

    websocket_->OnData([this](const char* data, size_t length, bool binary) {
        if (data == nullptr || length == 0) {
            return;
        }
        const auto* bytes = reinterpret_cast<const uint8_t*>(data);
        if (binary) {
            ESP_LOGI(TAG, "WS RX binary: seq=%lu, bytes=%u",
                     static_cast<unsigned long>(++rx_binary_sequence_),
                     static_cast<unsigned>(length));
            if (on_binary_ != nullptr) {
                on_binary_(std::vector<uint8_t>(bytes, bytes + length));
            }
        } else if (on_text_ != nullptr) {
            on_text_(std::string(data, length));
        }
    });

    websocket_->OnConnected([this]() {
        ESP_LOGI(TAG, "WebSocket transport connected");
        if (on_connected_ != nullptr) {
            on_connected_();
        }
    });


    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "WebSocket transport disconnected");
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
    });

    websocket_->OnError([this](int error) {
        ESP_LOGE(TAG, "Managed WebSocket error: %d", error);
        if (on_error_ != nullptr) {
            on_error_(error);
        }
    });

    ESP_LOGI(TAG, "Connecting to WebSocket server: %s", url.c_str());
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to WebSocket server");
        websocket_.reset();
        return false;
    }
    return true;
}

void XiaolingConnWebSocket::Disconnect() {
    if (websocket_ == nullptr) {
        return;
    }
    websocket_->Close();
    websocket_.reset();
}

bool XiaolingConnWebSocket::IsConnected() const {
    return websocket_ != nullptr && websocket_->IsConnected();
}

bool XiaolingConnWebSocket::SendText(const std::string& text) {
    if (!IsConnected()) {
        return false;
    }
    // Control messages are short and retain a tight deadline.
    RealtimeTransportScope deadline(kControlSendTimeoutMs);
    return websocket_->Send(text);
}

bool XiaolingConnWebSocket::SendBinary(const uint8_t* data, size_t size) {
    if (!IsConnected()) {
        ESP_LOGW(TAG, "WS TX binary skipped: transport is disconnected, bytes=%u",
                 static_cast<unsigned>(size));
        return false;
    }
    const uint32_t sequence = ++tx_binary_sequence_;
    const bool trace_send = sequence <= 3 || (sequence % 50) == 1;
    const int64_t started_us = esp_timer_get_time();
    if (trace_send) {
        ESP_LOGI(TAG, "上行链路[Socket写入开始]: seq=%lu, bytes=%u",
                 static_cast<unsigned long>(sequence), static_cast<unsigned>(size));
    }
    // Audio frames may share the transport with concurrent HTTPS traffic.
    // Allow transient congestion to recover before tearing down the WS.
    RealtimeTransportScope deadline(kBinarySendTimeoutMs);
    if (!websocket_->Send(data, size, true)) {
        ESP_LOGE(TAG, "WS TX binary failed: seq=%lu, bytes=%u",
                 static_cast<unsigned long>(sequence),
                 static_cast<unsigned>(size));
        LogBinarySendDiagnostics(sequence, size, started_us);
        NetworkDiagnostics::LogSnapshot("ws_binary_send_failed");
        NetworkDiagnostics::StartFailureProbe(
            endpoint_url_, "ws_binary_send_failed", sequence, size);
        return false;
    }
    const uint32_t elapsed_ms = static_cast<uint32_t>(
        (esp_timer_get_time() - started_us) / 1000);
    if (elapsed_ms >= 80) {
        ESP_LOGW(TAG, "上行链路[Socket写入结束]: seq=%lu, bytes=%u, wait=%lums",
                 static_cast<unsigned long>(sequence), static_cast<unsigned>(size),
                 static_cast<unsigned long>(elapsed_ms));
    } else if (trace_send) {
        ESP_LOGI(TAG, "上行链路[Socket写入结束]: seq=%lu, bytes=%u, wait=%lums",
                 static_cast<unsigned long>(sequence), static_cast<unsigned>(size),
                 static_cast<unsigned long>(elapsed_ms));
    }
    return true;
}

bool XiaolingConnWebSocket::Ping() {
    if (!IsConnected()) {
        return false;
    }
    RealtimeTransportScope deadline(kControlSendTimeoutMs);
    websocket_->Ping();
    return true;
}

void XiaolingConnWebSocket::OnText(std::function<void(std::string)> callback) {
    on_text_ = std::move(callback);
}

void XiaolingConnWebSocket::OnBinary(std::function<void(std::vector<uint8_t>)> callback) {
    on_binary_ = std::move(callback);
}


void XiaolingConnWebSocket::OnConnected(std::function<void()> callback) {
    on_connected_ = std::move(callback);
}

void XiaolingConnWebSocket::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = std::move(callback);
}

void XiaolingConnWebSocket::OnError(std::function<void(int)> callback) {
    on_error_ = std::move(callback);
}
