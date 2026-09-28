#include "src/esp/esp_tcp.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
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

struct WaitWritableResult {
    int select_result = -1;
    bool writable = false;
    bool exceptional = false;
    int select_errno = 0;

    bool CanWrite() const {
        return select_result > 0 && writable && !exceptional;
    }
};

WaitWritableResult WaitWritable(int fd, int timeout_ms) {
    fd_set write_set;
    fd_set exception_set;
    FD_ZERO(&write_set);
    FD_ZERO(&exception_set);
    FD_SET(fd, &write_set);
    FD_SET(fd, &exception_set);
    timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    WaitWritableResult result;
    result.select_result =
        select(fd + 1, nullptr, &write_set, &exception_set, &timeout);
    if (result.select_result < 0) {
        result.select_errno = errno;
    } else if (result.select_result > 0) {
        result.writable = FD_ISSET(fd, &write_set);
        result.exceptional = FD_ISSET(fd, &exception_set);
    }
    return result;
}

void FormatEndpoint(int fd, bool peer, char* output, size_t output_size) {
    sockaddr_in address = {};
    socklen_t address_size = sizeof(address);
    const int result = peer
        ? getpeername(fd, reinterpret_cast<sockaddr*>(&address), &address_size)
        : getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size);
    if (result != 0 || address.sin_family != AF_INET) {
        std::snprintf(output, output_size, "unavailable(errno=%d)", errno);
        return;
    }

    char ip[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)) == nullptr) {
        std::snprintf(output, output_size, "unavailable(errno=%d)", errno);
        return;
    }
    std::snprintf(output, output_size, "%s:%u", ip,
                  static_cast<unsigned>(ntohs(address.sin_port)));
}

void LogSocketDiagnostics(int fd, const char* stage,
                          const WaitWritableResult& wait_result,
                          int operation_errno) {
    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    const int socket_error_result =
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size);
    const int socket_error_query_errno = socket_error_result == 0 ? 0 : errno;

    char local_endpoint[64] = {};
    char peer_endpoint[64] = {};
    FormatEndpoint(fd, false, local_endpoint, sizeof(local_endpoint));
    FormatEndpoint(fd, true, peer_endpoint, sizeof(peer_endpoint));

    ESP_LOGW(TAG,
             "TCP send diagnostics: stage=%s, fd=%d, select_ret=%d, "
             "writable=%d, exceptional=%d, select_errno=%d(%s), "
             "operation_errno=%d(%s), so_error=%d(%s), "
             "so_error_query_errno=%d(%s), local=%s, peer=%s",
             stage, fd, wait_result.select_result, wait_result.writable,
             wait_result.exceptional, wait_result.select_errno,
             std::strerror(wait_result.select_errno), operation_errno,
             std::strerror(operation_errno), socket_error,
             std::strerror(socket_error), socket_error_query_errno,
             std::strerror(socket_error_query_errno), local_endpoint,
             peer_endpoint);
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
    const int64_t dns_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "TCP DNS lookup start: host=%s", host.c_str());
    hostent* server = gethostbyname(host.c_str());
    if (server == nullptr) {
        ESP_LOGE(TAG, "TCP DNS lookup failed: host=%s, elapsed=%ldms",
                 host.c_str(), static_cast<long>(
                     (esp_timer_get_time() - dns_started_us) / 1000));
        return false;
    }
    std::memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);
    char resolved_ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &server_addr.sin_addr, resolved_ip, sizeof(resolved_ip));
    ESP_LOGI(TAG,
             "TCP DNS lookup success: host=%s, address=%s, elapsed=%ldms",
             host.c_str(), resolved_ip, static_cast<long>(
                 (esp_timer_get_time() - dns_started_us) / 1000));

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno=%d(%s)", errno,
                 std::strerror(errno));
        return false;
    }
    const int64_t connect_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "TCP connect start: fd=%d, peer=%s:%d", tcp_fd_,
             resolved_ip, port);
    if (connect(tcp_fd_, reinterpret_cast<sockaddr*>(&server_addr),
                sizeof(server_addr)) < 0) {
        const int connect_errno = errno;
        ESP_LOGE(TAG,
                 "TCP connect failed: host=%s, address=%s:%d, elapsed=%ldms, "
                 "errno=%d(%s)",
                 host.c_str(), resolved_ip, port, static_cast<long>(
                     (esp_timer_get_time() - connect_started_us) / 1000),
                 connect_errno, std::strerror(connect_errno));
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }
    ESP_LOGI(TAG,
             "TCP connect success: fd=%d, host=%s, address=%s:%d, elapsed=%ldms",
             tcp_fd_, host.c_str(), resolved_ip, port, static_cast<long>(
                 (esp_timer_get_time() - connect_started_us) / 1000));

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
        WaitWritableResult wait_result;
        const bool deadline_expired = remaining_us <= 0;
        if (!deadline_expired) {
            wait_result = WaitWritable(
                tcp_fd_, std::max<int>(1, remaining_us / 1000));
        }
        if (deadline_expired || !wait_result.CanWrite()) {
            const char* stage = deadline_expired
                ? "deadline_before_select"
                : (wait_result.select_result == 0
                       ? "select_timeout"
                       : (wait_result.select_result < 0
                              ? "select_error"
                              : "socket_exception"));
            const int operation_errno = wait_result.select_errno != 0
                ? wait_result.select_errno
                : ETIMEDOUT;
            errno = ETIMEDOUT;
            ESP_LOGW(TAG, "TCP send deadline exceeded: sent=%u/%u, timeout=%dms",
                     static_cast<unsigned>(total_sent),
                     static_cast<unsigned>(data.size()), timeout_ms);
            LogSocketDiagnostics(tcp_fd_, stage, wait_result,
                                 operation_errno);
            return -1;
        }
        const int ret = send(tcp_fd_, data.data() + total_sent,
                             data.size() - total_sent, 0);
        if (ret <= 0) {
            const int operation_errno = errno;
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d(%s)", ret,
                     operation_errno, std::strerror(operation_errno));
            LogSocketDiagnostics(tcp_fd_, "send_error", wait_result,
                                 operation_errno);
            errno = operation_errno;
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
