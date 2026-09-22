#include "src/esp/esp_tcp.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_timer.h>

extern "C" int xiaoling_transport_current_send_timeout_ms();

namespace {

constexpr int kDefaultSendTimeoutMs = 2000;
constexpr int kDisconnectWaitMs = 2000;
const char* TAG = "EspTcp";

int CurrentSendTimeoutMs() {
    const int realtime_timeout = xiaoling_transport_current_send_timeout_ms();
    return realtime_timeout > 0 ? realtime_timeout : kDefaultSendTimeoutMs;
}

bool WaitWritable(int fd, int timeout_ms) {
    fd_set write_set;
    fd_set error_set;
    FD_ZERO(&write_set);
    FD_ZERO(&error_set);
    FD_SET(fd, &write_set);
    FD_SET(fd, &error_set);
    timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    const int result = select(fd + 1, nullptr, &write_set, &error_set, &timeout);
    return result > 0 && FD_ISSET(fd, &write_set) && !FD_ISSET(fd, &error_set);
}

}  // namespace

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    Disconnect();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspTcp::Connect(const std::string& host, int port) {
    if (connected_) {
        Disconnect();
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    hostent* server = gethostbyname(host.c_str());
    if (server == nullptr) {
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    std::memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }
    if (connect(tcp_fd_, reinterpret_cast<sockaddr*>(&server_addr),
                sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d", host.c_str(), port);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }

    connected_ = true;
    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    if (xTaskCreate([](void* arg) {
            auto* tcp = static_cast<EspTcp*>(arg);
            tcp->ReceiveTask();
            xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
            vTaskDelete(nullptr);
        }, "tcp_receive", 4096, this, 1, &receive_task_handle_) != pdPASS) {
        connected_ = false;
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }
    return true;
}

void EspTcp::Disconnect() {
    connected_ = false;
    if (tcp_fd_ == -1) {
        return;
    }
    shutdown(tcp_fd_, SHUT_RDWR);
    close(tcp_fd_);
    tcp_fd_ = -1;

    if (receive_task_handle_ != nullptr) {
        const auto bits = xEventGroupWaitBits(
            event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(kDisconnectWaitMs));
        if (!(bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT) && receive_task_handle_ != nullptr) {
            ESP_LOGE(TAG, "TCP receive task did not exit after socket shutdown; forcing stop");
            vTaskDelete(receive_task_handle_);
        }
        receive_task_handle_ = nullptr;
    }
}

int EspTcp::Send(const std::string& data) {
    if (!connected_ || tcp_fd_ < 0) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    const int timeout_ms = CurrentSendTimeoutMs();
    const int64_t deadline_us = esp_timer_get_time() +
                                static_cast<int64_t>(timeout_ms) * 1000;
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0 ||
            !WaitWritable(tcp_fd_, std::max<int>(1, remaining_us / 1000))) {
            errno = ETIMEDOUT;
            ESP_LOGW(TAG, "TCP send deadline exceeded: sent=%u/%u, timeout=%dms",
                     static_cast<unsigned>(total_sent),
                     static_cast<unsigned>(data.size()), timeout_ms);
            return -1;
        }
        const int ret = send(tcp_fd_, data.data() + total_sent,
                             data.size() - total_sent, 0);
        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }
        total_sent += static_cast<size_t>(ret);
    }
    return static_cast<int>(total_sent);
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        const int ret = recv(tcp_fd_, data.data(), data.size(), 0);
        if (ret <= 0) {
            if (ret < 0 && connected_) {
                ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            }
            const bool notify = connected_;
            connected_ = false;
            if (notify && disconnect_callback_) {
                disconnect_callback_();
            }
            break;
        }
        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}
