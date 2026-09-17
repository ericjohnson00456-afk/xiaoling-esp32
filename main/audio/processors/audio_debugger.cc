#include "audio_debugger.h"
#include "sdkconfig.h"

#if CONFIG_USE_AUDIO_DEBUGGER
#include <esp_log.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <string>
#if CONFIG_ESP_WIFI_ENABLED
#include <esp_wifi.h>
#endif
#endif

#define TAG "AudioDebugger"


AudioDebugger::AudioDebugger() {
#if CONFIG_USE_AUDIO_DEBUGGER
    std::string server_addr = CONFIG_AUDIO_DEBUG_UDP_SERVER;
    const size_t colon_pos = server_addr.find(':');
    if (colon_pos != std::string::npos) {
        const std::string ip = server_addr.substr(0, colon_pos);
        const int port = std::stoi(server_addr.substr(colon_pos + 1));

        memset(&udp_server_addr_, 0, sizeof(udp_server_addr_));
        udp_server_addr_.sin_family = AF_INET;
        udp_server_addr_.sin_port = htons(port);
        server_addr_valid_ = inet_pton(AF_INET, ip.c_str(), &udp_server_addr_.sin_addr) == 1;
    }
    if (!server_addr_valid_) {
        ESP_LOGW(TAG, "Invalid server address: %s, should be IP:PORT", CONFIG_AUDIO_DEBUG_UDP_SERVER);
    } else {
        ESP_LOGI(TAG, "Configured server address: %s", CONFIG_AUDIO_DEBUG_UDP_SERVER);
    }
#endif
}

AudioDebugger::~AudioDebugger() {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (udp_sockfd_ >= 0) {
        close(udp_sockfd_);
        ESP_LOGI(TAG, "Closed UDP socket");
    }
#endif
}

bool AudioDebugger::EnsureSocket() {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (udp_sockfd_ >= 0) {
        return true;
    }
    if (!server_addr_valid_) {
        return false;
    }
#if CONFIG_ESP_WIFI_ENABLED
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return false;
    }
#endif

    udp_sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sockfd_ < 0) {
        ESP_LOGW(TAG, "Failed to create UDP socket: %d", errno);
        return false;
    }
    int enable_broadcast = 1;
    if (setsockopt(udp_sockfd_, SOL_SOCKET, SO_BROADCAST,
                   &enable_broadcast, sizeof(enable_broadcast)) < 0) {
        ESP_LOGW(TAG, "Failed to enable UDP broadcast: %d", errno);
    }
    ESP_LOGI(TAG, "Audio debug socket ready: %s", CONFIG_AUDIO_DEBUG_UDP_SERVER);
    return true;
#else
    return false;
#endif
}

void AudioDebugger::Feed(const std::vector<int16_t>& data) {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (data.empty() || !EnsureSocket()) {
        return;
    }
    for (const int16_t sample : data) {
        pending_audio_[pending_samples_++] = sample;
        if (pending_samples_ == pending_audio_.size()) {
            SendPendingAudio();
        }
    }
#endif
}

void AudioDebugger::SendPendingAudio() {
#if CONFIG_USE_AUDIO_DEBUGGER
    const ssize_t sent = sendto(udp_sockfd_, pending_audio_.data(),
                                pending_samples_ * sizeof(int16_t), 0,
                                (struct sockaddr*)&udp_server_addr_, sizeof(udp_server_addr_));
    if (sent < 0) {
        static uint32_t failed_packets = 0;
        if ((++failed_packets % 100) == 1) {
            ESP_LOGW(TAG, "Failed to send wake inference audio to %s: %d (failures=%lu)",
                     CONFIG_AUDIO_DEBUG_UDP_SERVER, errno,
                     static_cast<unsigned long>(failed_packets));
        }
    } else {
        ESP_LOGD(TAG, "Sent %d bytes wake inference audio to %s",
                 sent, CONFIG_AUDIO_DEBUG_UDP_SERVER);
    }
    pending_samples_ = 0;
#endif
}


