#include "src/esp/esp_ssl.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>

extern "C" int xiaoling_transport_current_send_timeout_ms();

namespace {

constexpr int kDefaultSendTimeoutMs = 2000;
constexpr int kDisconnectWaitMs = 2000;
const char* TAG = "EspSsl";

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

void ApplySendTimeout(int fd, int timeout_ms) {
    timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGW(TAG, "Failed to set TLS send timeout: errno=%d", errno);
    }
}

}  // namespace

EspSsl::EspSsl() {
    event_group_ = xEventGroupCreate();
}

EspSsl::~EspSsl() {
    Disconnect();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspSsl::Connect(const std::string& host, int port) {
    if (tls_client_ != nullptr) {
        ESP_LOGE(TAG, "tls client has been initialized");
        return false;
    }

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return false;
    }

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    const int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg,
                                          tls_client_);
    if (ret != 1) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d", host.c_str(), port);
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return false;
    }

    connected_ = true;
    xEventGroupClearBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
    if (xTaskCreate([](void* arg) {
            auto* ssl = static_cast<EspSsl*>(arg);
            ssl->ReceiveTask();
            xEventGroupSetBits(ssl->event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
            vTaskDelete(nullptr);
        }, "ssl_receive", 4096, this, 1, &receive_task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TLS receive task");
        connected_ = false;
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return false;
    }
    return true;
}

void EspSsl::Disconnect() {
    connected_ = false;
    if (tls_client_ == nullptr) {
        return;
    }

    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) == ESP_OK && sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
    }

    if (receive_task_handle_ != nullptr) {
        const auto bits = xEventGroupWaitBits(
            event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(kDisconnectWaitMs));
        if (!(bits & ESP_SSL_EVENT_RECEIVE_TASK_EXIT) && receive_task_handle_ != nullptr) {
            ESP_LOGE(TAG, "TLS receive task did not exit after socket shutdown; forcing stop");
            vTaskDelete(receive_task_handle_);
        }
        receive_task_handle_ = nullptr;
    }

    esp_tls_conn_destroy(tls_client_);
    tls_client_ = nullptr;
}

int EspSsl::Send(const std::string& data) {
    if (!connected_ || tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) != ESP_OK || sockfd < 0) {
        ESP_LOGE(TAG, "Failed to get TLS socket");
        return -1;
    }

    const int timeout_ms = CurrentSendTimeoutMs();
    const int64_t deadline_us = esp_timer_get_time() +
                                static_cast<int64_t>(timeout_ms) * 1000;
    ApplySendTimeout(sockfd, timeout_ms);

    size_t total_sent = 0;
    while (total_sent < data.size()) {
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0 ||
            !WaitWritable(sockfd, std::max<int>(1, remaining_us / 1000))) {
            errno = ETIMEDOUT;
            ESP_LOGW(TAG, "TLS send deadline exceeded: sent=%u/%u, timeout=%dms",
                     static_cast<unsigned>(total_sent),
                     static_cast<unsigned>(data.size()), timeout_ms);
            return -1;
        }

        const int ret = esp_tls_conn_write(tls_client_, data.data() + total_sent,
                                           data.size() - total_sent);
        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            taskYIELD();
            continue;
        }
        if (ret <= 0) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }
        total_sent += static_cast<size_t>(ret);
    }
    return static_cast<int>(total_sent);
}

void EspSsl::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        const int ret = esp_tls_conn_read(tls_client_, data.data(), data.size());
        if (ret == ESP_TLS_ERR_SSL_WANT_READ) {
            taskYIELD();
            continue;
        }
        if (ret <= 0) {
            if (ret < 0 && connected_) {
                ESP_LOGE(TAG, "SSL receive failed: %d", ret);
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
