#include "cloud_websocket.h"

#include "board.h"
#include "web_socket.h"

#include <esp_log.h>

#include <utility>

#define TAG "CloudWebSocket"

CloudWebSocket::CloudWebSocket(int connect_id)
    : connect_id_(connect_id) {
}

CloudWebSocket::~CloudWebSocket() {
    Disconnect();
}

bool CloudWebSocket::Connect(const std::string& url, const CloudWebSocketHeaders& headers) {
    Disconnect();
    rx_binary_sequence_ = 0;

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

void CloudWebSocket::Disconnect() {
    if (websocket_ == nullptr) {
        return;
    }
    websocket_->Close();
    websocket_.reset();
}

bool CloudWebSocket::IsConnected() const {
    return websocket_ != nullptr && websocket_->IsConnected();
}

bool CloudWebSocket::SendText(const std::string& text) {
    return IsConnected() && websocket_->Send(text);
}

bool CloudWebSocket::SendBinary(const uint8_t* data, size_t size) {
    if (!IsConnected()) {
        ESP_LOGW(TAG, "WS TX binary skipped: transport is disconnected, bytes=%u",
                 static_cast<unsigned>(size));
        return false;
    }
    if (!websocket_->Send(data, size, true)) {
        ESP_LOGE(TAG, "WS TX binary failed: bytes=%u", static_cast<unsigned>(size));
        return false;
    }
    return true;
}

bool CloudWebSocket::Ping() {
    if (!IsConnected()) {
        return false;
    }
    websocket_->Ping();
    return true;
}

void CloudWebSocket::OnText(std::function<void(std::string)> callback) {
    on_text_ = std::move(callback);
}

void CloudWebSocket::OnBinary(std::function<void(std::vector<uint8_t>)> callback) {
    on_binary_ = std::move(callback);
}


void CloudWebSocket::OnConnected(std::function<void()> callback) {
    on_connected_ = std::move(callback);
}

void CloudWebSocket::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = std::move(callback);
}

void CloudWebSocket::OnError(std::function<void(int)> callback) {
    on_error_ = std::move(callback);
}
