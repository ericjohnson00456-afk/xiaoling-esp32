#ifndef XIAOLING_CONN_H
#define XIAOLING_CONN_H

#include "xiaoling_conn_websocket.h"
#include "xiaoling_types.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

class XiaolingSessionVoice;

enum class XiaolingError {
    ServerNotConnected,
    ServerTimeout,
    ServerError,
};

// 小聆连接入口：负责鉴权、长连接、心跳、重连和语音模块接入。
class XiaolingConn final {
public:
    XiaolingConn();
    ~XiaolingConn();

    // 启动即完成鉴权和连接；连接成功后语音模块可以直接开始会话。
    bool Start();
    bool Connect();
    void Disconnect();
    bool IsConnected() const;
    bool IsSessionActive() const;
    bool CanUploadAudio() const;

    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet);
    void OnAudioSent(std::function<void(uint32_t)> callback);
    void SendWakeWordDetected(const std::string& wake_word);
    void SendStartListening(ListeningMode mode);
    void SendStopListening();
    bool EndSession();
    void PrepareAbortSpeaking(AbortReason reason);
    void SendAbortSpeaking(AbortReason reason);
    void SendMcpMessage(const std::string& message);
    bool ResolveKuwoMusicUrl(const std::string& item_id, std::string& url);
    void SetNarrowbandMode(bool enabled);

    int server_sample_rate() const;
    int server_frame_duration() const;
    const std::string& session_id() const;

    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket>)> callback);
    void OnIncomingJson(std::function<void(const cJSON*)> callback);
    void OnNetworkError(std::function<void(XiaolingError)> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);

private:
    static constexpr uint32_t kHeartbeatIntervalMs = 30000;
    static constexpr uint32_t kReconnectIntervalMs = 3000;
    static constexpr uint32_t kProtocolConnectTimeoutMs = 10000;
    static constexpr EventBits_t kProtocolConnectedBit = BIT0;
    static constexpr size_t kMaxUplinkPackets = 25;
    static constexpr size_t kUplinkBatchPackets = 5;
    static constexpr uint32_t kUplinkBatchWaitMs = 40;

    std::unique_ptr<XiaolingSessionVoice> voice_;
    EventGroupHandle_t event_group_ = nullptr;

    mutable std::mutex websocket_mutex_;
    std::unique_ptr<XiaolingConnWebSocket> websocket_;
    std::atomic_bool running_{false};
    std::atomic_bool desired_connected_{false};
    std::atomic_bool transport_connected_{false};
    std::atomic_bool protocol_connected_{false};
    std::atomic_bool session_active_{false};
    std::atomic_bool connecting_{false};
    std::atomic<uint32_t> generation_{0};
    uint32_t reconnect_attempts_ = 0;
    bool disconnected_event_published_ = false;
    TaskHandle_t task_handle_ = nullptr;
    int64_t last_heartbeat_us_ = 0;

    std::mutex uplink_mutex_;
    std::deque<std::unique_ptr<AudioStreamPacket>> uplink_queue_;
    uint32_t dropped_uplink_packets_ = 0;

    mutable std::mutex auth_mutex_;
    std::string access_token_;
    bool error_occurred_ = false;

    std::function<void(const cJSON*)> on_incoming_json_;
    std::function<void(uint32_t)> on_audio_sent_;
    std::function<void(XiaolingError)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    bool StartSupervisor();
    void StopSupervisor();
    static void TaskEntry(void* arg);
    void Run();
    bool ConnectOnce();
    void HandleTransportDisconnected(uint32_t generation);
    void ForceReconnect();
    void PublishDisconnected();
    void SetSessionActive(bool active);
    bool SendPendingAudio();
    void ClearUplinkQueue();

    bool EnsureAuthenticated();
    bool Authenticate(const std::string& extra_code = "");
    void ClearAuthentication();
    std::string LoadAccessToken() const;
    static std::string NormalizeBearerToken(const std::string& token);

    bool SendText(const std::string& text);
    bool SendBinary(const uint8_t* data, size_t size);
    std::string BuildWebSocketUrl() const;
    void HandleIncomingText(const std::string& text);
    bool IsCurrentSessionMessage(const cJSON* root) const;
    void SetError(XiaolingError error);
};

#endif  // XIAOLING_CONN_H
