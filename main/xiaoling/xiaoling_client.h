#ifndef XIAOLING_CLIENT_H
#define XIAOLING_CLIENT_H

#include "xiaoling_types.h"

#include <cJSON.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

class CloudWebSocket;

// 小聆产品客户端：负责 lschat 鉴权、会话控制和消息路由。
// CloudHttp/CloudWebSocket 只提供网络传输，不感知 lschat 消息格式。
class XiaolingClient final {
public:
    XiaolingClient();
    ~XiaolingClient();

    bool Start();
    bool OpenAudioChannel();
    void CloseAudioChannel();
    bool IsAudioChannelOpened() const;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet);

    void SetNarrowbandMode(bool enabled);

    int server_sample_rate() const { return server_sample_rate_; }
    int server_frame_duration() const { return server_frame_duration_; }
    const std::string& session_id() const { return session_id_; }

    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket>)> callback);
    void OnIncomingJson(std::function<void(const cJSON*)> callback);
    void OnAudioChannelOpened(std::function<void()> callback);
    void OnAudioChannelClosed(std::function<void()> callback);
    void OnNetworkError(std::function<void(const std::string&)> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);

    void SendWakeWordDetected(const std::string& wake_word);
    void SendStartListening(ListeningMode mode);
    void SendStopListening();
    void SendAbortSpeaking(AbortReason reason);
    void SendMcpMessage(const std::string& message);

private:
    std::unique_ptr<CloudWebSocket> websocket_;
    EventGroupHandle_t event_group_ = nullptr;
    std::atomic_bool protocol_connected_{false};
    std::atomic_bool session_active_{false};
    std::atomic_bool audio_upload_enabled_{false};
    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    bool narrowband_mode_ = false;
    bool error_occurred_ = false;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    std::string session_id_;
    std::string pending_wake_word_;
    std::deque<std::unique_ptr<AudioStreamPacket>> pending_wake_audio_;
    bool uplink_audio_format_checked_ = false;
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;

    std::function<void(const cJSON*)> on_incoming_json_;
    std::function<void(std::unique_ptr<AudioStreamPacket>)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string&)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    void SetError(const std::string& message);
    bool IsTimeout() const;
    bool EnsureAuthenticated();
    bool SendText(const std::string& text);
    std::string BuildWebSocketUrl() const;
    std::string BuildStartMessage(ListeningMode mode);
    void HandleIncomingText(const std::string& text);
    void HandleIncomingBinary(std::vector<uint8_t> payload);
    bool CheckUplinkAudioFormat(const AudioStreamPacket& packet);
    bool SendUplinkAudioPacket(const AudioStreamPacket& packet);
    bool FlushPendingWakeAudio();
    bool IsCurrentSessionMessage(const cJSON* root) const;
};

#endif // XIAOLING_CLIENT_H
