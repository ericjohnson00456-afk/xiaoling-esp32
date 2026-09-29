#include "network_diagnostics.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <new>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <lwip/ip_addr.h>
#include <ping/ping_sock.h>

namespace NetworkDiagnostics {
namespace {

constexpr EventBits_t kPingFinishedBit = BIT0;
constexpr uint32_t kProbePingCount = 3;
constexpr uint32_t kProbePingIntervalMs = 200;
constexpr uint32_t kProbePingTimeoutMs = 500;
constexpr int kProbeTcpTimeoutMs = 3000;
constexpr uint32_t kProbeTaskStackSize = 6144;
constexpr UBaseType_t kProbeTaskPriority = 3;
const char* TAG = "NetDiag";

std::mutex g_initialization_mutex;
bool g_initialized = false;
esp_event_handler_instance_t g_wifi_event_instance = nullptr;
esp_event_handler_instance_t g_ip_event_instance = nullptr;
std::atomic<bool> g_probe_running{false};

struct Endpoint {
    std::string host;
    uint16_t port = 0;
};

struct FailureProbeRequest {
    std::string url;
    std::string reason;
    uint32_t sequence = 0;
    size_t payload_size = 0;
};

const char* WifiReasonName(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED:
            return "unspecified";
        case WIFI_REASON_AUTH_EXPIRE:
            return "auth_expire";
        case WIFI_REASON_AUTH_LEAVE:
            return "auth_leave";
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
            return "inactivity";
        case WIFI_REASON_NOT_ENOUGH_BANDWIDTH:
            return "not_enough_bandwidth";
        case WIFI_REASON_MISSING_ACKS:
            return "missing_acks";
        case WIFI_REASON_EXCEEDED_TXOP:
            return "exceeded_txop";
        case WIFI_REASON_END_BA:
            return "end_block_ack";
        case WIFI_REASON_UNKNOWN_BA:
            return "unknown_block_ack";
        case WIFI_REASON_TIMEOUT:
            return "timeout";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "beacon_timeout";
        case WIFI_REASON_NO_AP_FOUND:
            return "no_ap_found";
        case WIFI_REASON_AUTH_FAIL:
            return "auth_fail";
        case WIFI_REASON_ASSOC_FAIL:
            return "assoc_fail";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "handshake_timeout";
        case WIFI_REASON_CONNECTION_FAIL:
            return "connection_fail";
        case WIFI_REASON_AP_TSF_RESET:
            return "ap_tsf_reset";
        case WIFI_REASON_ROAMING:
            return "roaming";
        default:
            return "other";
    }
}

const char* WifiPowerSaveName(wifi_ps_type_t power_save) {
    switch (power_save) {
        case WIFI_PS_NONE:
            return "none";
        case WIFI_PS_MIN_MODEM:
            return "min_modem";
        case WIFI_PS_MAX_MODEM:
            return "max_modem";
        default:
            return "unknown";
    }
}

void FormatIpv4(const esp_ip4_addr_t& address, char* output,
                size_t output_size) {
    if (esp_ip4addr_ntoa(&address, output, output_size) == nullptr) {
        std::snprintf(output, output_size, "unavailable");
    }
}

void WifiEventHandler(void*, esp_event_base_t, int32_t event_id,
                      void* event_data) {
    switch (event_id) {
        case WIFI_EVENT_STA_CONNECTED: {
            const auto* event =
                static_cast<const wifi_event_sta_connected_t*>(event_data);
            ESP_LOGI(TAG,
                     "WiFi event: sta_connected, bssid=%02x:%02x:%02x:%02x:%02x:%02x, "
                     "channel=%u, authmode=%d, aid=%u",
                     event->bssid[0], event->bssid[1], event->bssid[2],
                     event->bssid[3], event->bssid[4], event->bssid[5],
                     static_cast<unsigned>(event->channel),
                     static_cast<int>(event->authmode),
                     static_cast<unsigned>(event->aid));
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const auto* event =
                static_cast<const wifi_event_sta_disconnected_t*>(event_data);
            ESP_LOGW(TAG,
                     "WiFi event: sta_disconnected, reason=%u(%s), rssi=%d dBm, "
                     "bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                     static_cast<unsigned>(event->reason),
                     WifiReasonName(event->reason), static_cast<int>(event->rssi),
                     event->bssid[0], event->bssid[1], event->bssid[2],
                     event->bssid[3], event->bssid[4], event->bssid[5]);
            break;
        }
        case WIFI_EVENT_STA_BSS_RSSI_LOW: {
            const auto* event =
                static_cast<const wifi_event_bss_rssi_low_t*>(event_data);
            ESP_LOGW(TAG, "WiFi event: bss_rssi_low, rssi=%ld dBm",
                     static_cast<long>(event->rssi));
            break;
        }
        case WIFI_EVENT_STA_BEACON_TIMEOUT:
            ESP_LOGW(TAG, "WiFi event: beacon_timeout");
            break;
        case WIFI_EVENT_HOME_CHANNEL_CHANGE: {
            const auto* event =
                static_cast<const wifi_event_home_channel_change_t*>(event_data);
            ESP_LOGW(TAG,
                     "WiFi event: home_channel_change, old=%u/%d, new=%u/%d",
                     static_cast<unsigned>(event->old_chan),
                     static_cast<int>(event->old_snd),
                     static_cast<unsigned>(event->new_chan),
                     static_cast<int>(event->new_snd));
            break;
        }
        case WIFI_EVENT_STA_STOP:
            ESP_LOGW(TAG, "WiFi event: sta_stop");
            break;
        default:
            break;
    }
}

void IpEventHandler(void*, esp_event_base_t, int32_t event_id,
                    void* event_data) {
    if (event_id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);
        char ip[16] = {};
        char gateway[16] = {};
        char netmask[16] = {};
        FormatIpv4(event->ip_info.ip, ip, sizeof(ip));
        FormatIpv4(event->ip_info.gw, gateway, sizeof(gateway));
        FormatIpv4(event->ip_info.netmask, netmask, sizeof(netmask));
        ESP_LOGI(TAG,
                 "IP event: got_ip, ip=%s, gateway=%s, netmask=%s, changed=%d",
                 ip, gateway, netmask, event->ip_changed);
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "IP event: sta_lost_ip");
    }
}

bool ParseEndpoint(const std::string& url, Endpoint* endpoint) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    const std::string scheme = url.substr(0, scheme_end);
    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = url.find_first_of("/?#", authority_start);
    const std::string authority = url.substr(
        authority_start, authority_end == std::string::npos
                             ? std::string::npos
                             : authority_end - authority_start);
    if (authority.empty() || authority.front() == '[') {
        return false;
    }

    endpoint->port = scheme == "wss" || scheme == "https" ? 443 : 80;
    const size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        endpoint->host = authority;
        return true;
    }

    endpoint->host = authority.substr(0, colon);
    const char* port_text = authority.c_str() + colon + 1;
    char* port_end = nullptr;
    const long parsed_port = std::strtol(port_text, &port_end, 10);
    if (endpoint->host.empty() || port_end == port_text || *port_end != '\0' ||
        parsed_port <= 0 || parsed_port > 65535) {
        return false;
    }
    endpoint->port = static_cast<uint16_t>(parsed_port);
    return true;
}

bool GetStationNetworkInfo(esp_netif_ip_info_t* ip_info,
                           esp_netif_dns_info_t* dns_info) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        return false;
    }
    if (esp_netif_get_ip_info(netif, ip_info) != ESP_OK) {
        return false;
    }
    if (dns_info != nullptr) {
        *dns_info = {};
        esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, dns_info);
    }
    return true;
}

void PingFinished(esp_ping_handle_t, void* args) {
    xEventGroupSetBits(static_cast<EventGroupHandle_t>(args),
                       kPingFinishedBit);
}

void RunGatewayPing(const esp_ip4_addr_t& gateway) {
    ip_addr_t target = {};
    IP_SET_TYPE_VAL(target, IPADDR_TYPE_V4);
    ip4_addr_copy(*ip_2_ip4(&target), gateway);

    EventGroupHandle_t finished = xEventGroupCreate();
    if (finished == nullptr) {
        ESP_LOGE(TAG, "Failure probe: gateway ping event group allocation failed");
        return;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    // Three short samples keep the diagnostic below two seconds while still
    // distinguishing a single dropped frame from a consistently unreachable AP.
    config.count = kProbePingCount;
    config.interval_ms = kProbePingIntervalMs;
    config.timeout_ms = kProbePingTimeoutMs;
    config.target_addr = target;

    esp_ping_callbacks_t callbacks = {};
    callbacks.cb_args = finished;
    callbacks.on_ping_end = PingFinished;

    esp_ping_handle_t ping = nullptr;
    const esp_err_t create_result =
        esp_ping_new_session(&config, &callbacks, &ping);
    if (create_result != ESP_OK) {
        ESP_LOGE(TAG, "Failure probe: gateway ping create failed: %s",
                 esp_err_to_name(create_result));
        vEventGroupDelete(finished);
        return;
    }

    const int64_t started_us = esp_timer_get_time();
    const esp_err_t start_result = esp_ping_start(ping);
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "Failure probe: gateway ping start failed: %s",
                 esp_err_to_name(start_result));
        esp_ping_delete_session(ping);
        vEventGroupDelete(finished);
        return;
    }

    const uint32_t maximum_wait_ms =
        kProbePingCount * (kProbePingIntervalMs + kProbePingTimeoutMs) + 1000;
    const EventBits_t bits = xEventGroupWaitBits(
        finished, kPingFinishedBit, pdTRUE, pdTRUE,
        pdMS_TO_TICKS(maximum_wait_ms));

    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t duration_ms = 0;
    esp_ping_get_profile(ping, ESP_PING_PROF_REQUEST, &transmitted,
                         sizeof(transmitted));
    esp_ping_get_profile(ping, ESP_PING_PROF_REPLY, &received,
                         sizeof(received));
    esp_ping_get_profile(ping, ESP_PING_PROF_DURATION, &duration_ms,
                         sizeof(duration_ms));

    char gateway_text[16] = {};
    FormatIpv4(gateway, gateway_text, sizeof(gateway_text));
    ESP_LOGW(TAG,
             "Failure probe: gateway_ping, target=%s, completed=%d, tx=%lu, "
             "rx=%lu, duration=%lums, wall=%ldms",
             gateway_text, (bits & kPingFinishedBit) != 0,
             static_cast<unsigned long>(transmitted),
             static_cast<unsigned long>(received),
             static_cast<unsigned long>(duration_ms),
             static_cast<long>((esp_timer_get_time() - started_us) / 1000));

    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    vEventGroupDelete(finished);
}

bool ResolveEndpoint(const Endpoint& endpoint, sockaddr_in* address) {
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char service[8] = {};
    std::snprintf(service, sizeof(service), "%u",
                  static_cast<unsigned>(endpoint.port));

    ESP_LOGW(TAG, "Failure probe: DNS start, host=%s, port=%u",
             endpoint.host.c_str(), static_cast<unsigned>(endpoint.port));
    const int64_t started_us = esp_timer_get_time();
    addrinfo* result = nullptr;
    const int dns_result =
        getaddrinfo(endpoint.host.c_str(), service, &hints, &result);
    const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (dns_result != 0 || result == nullptr) {
        ESP_LOGE(TAG,
                 "Failure probe: DNS failed, host=%s, result=%d, "
                 "elapsed=%ldms",
                 endpoint.host.c_str(), dns_result,
                 static_cast<long>(elapsed_ms));
        if (result != nullptr) {
            freeaddrinfo(result);
        }
        return false;
    }

    std::memcpy(address, result->ai_addr, sizeof(*address));
    char resolved_ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address->sin_addr, resolved_ip, sizeof(resolved_ip));
    ESP_LOGW(TAG,
             "Failure probe: DNS success, host=%s, address=%s:%u, "
             "elapsed=%ldms",
             endpoint.host.c_str(), resolved_ip,
             static_cast<unsigned>(ntohs(address->sin_port)),
             static_cast<long>(elapsed_ms));
    freeaddrinfo(result);
    return true;
}

void RunTcpConnectProbe(const Endpoint& endpoint,
                        const sockaddr_in& address) {
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failure probe: TCP socket failed, errno=%d(%s)", errno,
                 std::strerror(errno));
        return;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        const int saved_errno = errno;
        ESP_LOGE(TAG, "Failure probe: TCP nonblocking setup failed, errno=%d(%s)",
                 saved_errno, std::strerror(saved_errno));
        close(fd);
        return;
    }

    char resolved_ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address.sin_addr, resolved_ip, sizeof(resolved_ip));
    ESP_LOGW(TAG,
             "Failure probe: TCP connect start, host=%s, address=%s:%u, "
             "timeout=%dms",
             endpoint.host.c_str(), resolved_ip,
             static_cast<unsigned>(endpoint.port), kProbeTcpTimeoutMs);

    const int64_t started_us = esp_timer_get_time();
    int connect_result = connect(
        fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    int connect_errno = connect_result == 0 ? 0 : errno;
    int select_result = -1;
    bool writable = connect_result == 0;
    bool exceptional = false;

    if (connect_result != 0 &&
        (connect_errno == EINPROGRESS || connect_errno == EWOULDBLOCK)) {
        fd_set write_set;
        fd_set exception_set;
        FD_ZERO(&write_set);
        FD_ZERO(&exception_set);
        FD_SET(fd, &write_set);
        FD_SET(fd, &exception_set);
        timeval timeout = {
            .tv_sec = kProbeTcpTimeoutMs / 1000,
            .tv_usec = (kProbeTcpTimeoutMs % 1000) * 1000,
        };
        select_result =
            select(fd + 1, nullptr, &write_set, &exception_set, &timeout);
        if (select_result > 0) {
            writable = FD_ISSET(fd, &write_set);
            exceptional = FD_ISSET(fd, &exception_set);
        }
    }

    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    const int socket_error_result =
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size);
    const int socket_error_query_errno =
        socket_error_result == 0 ? 0 : errno;
    const bool connected =
        (connect_result == 0 || (select_result > 0 && writable)) &&
        !exceptional && socket_error_result == 0 && socket_error == 0;

    ESP_LOGW(TAG,
             "Failure probe: TCP connect result, connected=%d, elapsed=%ldms, "
             "connect_ret=%d, connect_errno=%d(%s), select_ret=%d, "
             "writable=%d, exceptional=%d, so_error=%d(%s), "
             "so_error_query_errno=%d(%s)",
             connected,
             static_cast<long>((esp_timer_get_time() - started_us) / 1000),
             connect_result, connect_errno, std::strerror(connect_errno),
             select_result, writable, exceptional, socket_error,
             std::strerror(socket_error), socket_error_query_errno,
             std::strerror(socket_error_query_errno));
    close(fd);
}

void RunFailureProbe(const FailureProbeRequest& request) {
    ESP_LOGW(TAG,
             "Failure probe started: reason=%s, seq=%lu, bytes=%u, url_bytes=%u",
             request.reason.c_str(), static_cast<unsigned long>(request.sequence),
             static_cast<unsigned>(request.payload_size),
             static_cast<unsigned>(request.url.size()));
    LogSnapshot("failure_probe_start");

    esp_netif_ip_info_t ip_info = {};
    esp_netif_dns_info_t dns_info = {};
    if (GetStationNetworkInfo(&ip_info, &dns_info) && ip_info.gw.addr != 0) {
        RunGatewayPing(ip_info.gw);
    } else {
        ESP_LOGE(TAG, "Failure probe: gateway unavailable");
    }

    Endpoint endpoint;
    if (!ParseEndpoint(request.url, &endpoint)) {
        ESP_LOGE(TAG, "Failure probe: unable to parse WebSocket endpoint");
        return;
    }

    sockaddr_in address = {};
    if (ResolveEndpoint(endpoint, &address)) {
        RunTcpConnectProbe(endpoint, address);
    }
    LogSnapshot("failure_probe_end");
    ESP_LOGW(TAG, "Failure probe finished: reason=%s, seq=%lu",
             request.reason.c_str(),
             static_cast<unsigned long>(request.sequence));
}

}  // namespace

void Initialize() {
    if (g_initialized) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_initialization_mutex);
    if (g_initialized) {
        return;
    }

    esp_err_t wifi_result = ESP_OK;
    esp_err_t ip_result = ESP_OK;
    if (g_wifi_event_instance == nullptr) {
        wifi_result = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, WifiEventHandler, nullptr,
            &g_wifi_event_instance);
    }
    if (g_ip_event_instance == nullptr) {
        ip_result = esp_event_handler_instance_register(
            IP_EVENT, ESP_EVENT_ANY_ID, IpEventHandler, nullptr,
            &g_ip_event_instance);
    }
    g_initialized = wifi_result == ESP_OK && ip_result == ESP_OK;
    if (g_initialized) {
        ESP_LOGI(TAG, "Network event diagnostics registered");
    } else {
        ESP_LOGE(TAG,
                 "Network event diagnostics registration failed: wifi=%s, ip=%s",
                 esp_err_to_name(wifi_result), esp_err_to_name(ip_result));
    }
}

void LogSnapshot(const char* reason) {
    wifi_ap_record_t ap_info = {};
    const esp_err_t ap_result = esp_wifi_sta_get_ap_info(&ap_info);
    wifi_ps_type_t power_save = WIFI_PS_NONE;
    const esp_err_t power_save_result = esp_wifi_get_ps(&power_save);
    wifi_config_t station_config = {};
    const esp_err_t config_result =
        esp_wifi_get_config(WIFI_IF_STA, &station_config);

    esp_netif_ip_info_t ip_info = {};
    esp_netif_dns_info_t dns_info = {};
    const bool have_network_info =
        GetStationNetworkInfo(&ip_info, &dns_info);
    char ip[16] = "unavailable";
    char gateway[16] = "unavailable";
    char dns[16] = "unavailable";
    if (have_network_info) {
        FormatIpv4(ip_info.ip, ip, sizeof(ip));
        FormatIpv4(ip_info.gw, gateway, sizeof(gateway));
        if (dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
            FormatIpv4(dns_info.ip.u_addr.ip4, dns, sizeof(dns));
        }
    }

    ESP_LOGW(TAG,
             "Network snapshot: reason=%s, ap=%s, rssi=%d dBm, channel=%u/%d, "
             "bssid=%02x:%02x:%02x:%02x:%02x:%02x, ps=%s(%d), "
             "listen_interval=%u, ip=%s, gateway=%s, dns=%s",
             reason, esp_err_to_name(ap_result),
             ap_result == ESP_OK ? static_cast<int>(ap_info.rssi) : 0,
             ap_result == ESP_OK ? static_cast<unsigned>(ap_info.primary) : 0,
             ap_result == ESP_OK ? static_cast<int>(ap_info.second) : 0,
             ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
             ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5],
             power_save_result == ESP_OK ? WifiPowerSaveName(power_save)
                                         : "unavailable",
             power_save_result == ESP_OK ? static_cast<int>(power_save) : -1,
             config_result == ESP_OK
                 ? static_cast<unsigned>(station_config.sta.listen_interval)
                 : 0,
             ip, gateway, dns);
    ESP_LOGW(TAG,
             "Runtime snapshot: internal_free=%u, internal_min=%u, "
             "internal_largest=%u, dma_free=%u, psram_free=%u, "
             "psram_largest=%u, task_stack_free=%u words",
             static_cast<unsigned>(heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(
                 MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void StartFailureProbe(const std::string& url, const char* reason,
                       uint32_t sequence, size_t payload_size) {
    bool expected = false;
    if (!g_probe_running.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG,
                 "Failure probe skipped: another probe is running, reason=%s, "
                 "seq=%lu",
                 reason, static_cast<unsigned long>(sequence));
        return;
    }

    auto* request = new (std::nothrow) FailureProbeRequest{
        .url = url,
        .reason = reason,
        .sequence = sequence,
        .payload_size = payload_size,
    };
    if (request == nullptr) {
        g_probe_running = false;
        ESP_LOGE(TAG, "Failure probe request allocation failed");
        return;
    }

    const BaseType_t task_result = xTaskCreate(
        [](void* arg) {
            std::unique_ptr<FailureProbeRequest> request(
                static_cast<FailureProbeRequest*>(arg));
            RunFailureProbe(*request);
            g_probe_running = false;
            vTaskDelete(nullptr);
        },
        "net_failure_probe", kProbeTaskStackSize, request,
        kProbeTaskPriority, nullptr);
    if (task_result != pdPASS) {
        delete request;
        g_probe_running = false;
        ESP_LOGE(TAG, "Failure probe task creation failed");
    }
}

}  // namespace NetworkDiagnostics
