#ifndef XIAOLING_CONN_WEBSOCKET_H
#define XIAOLING_CONN_WEBSOCKET_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using XiaolingWsHeaders = std::vector<std::pair<std::string, std::string>>;

// 小聆连接内部使用的 WebSocket 传输薄封装，不解析小聆业务消息。
class XiaolingConnWebSocket final {
public:
    explicit XiaolingConnWebSocket(int connect_id = 1);
    ~XiaolingConnWebSocket();

    bool Connect(const std::string& url, const XiaolingWsHeaders& headers = {});
    void Disconnect();
    bool IsConnected() const;

    bool SendText(const std::string& text);
    bool SendBinary(const uint8_t* data, size_t size);
    bool Ping();

    void OnText(std::function<void(std::string)> callback);
    void OnBinary(std::function<void(std::vector<uint8_t>)> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);
    void OnError(std::function<void(int)> callback);

private:
    int connect_id_;
    std::unique_ptr<class WebSocket> websocket_;
    std::function<void(std::string)> on_text_;
    std::function<void(std::vector<uint8_t>)> on_binary_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::function<void(int)> on_error_;
    uint32_t rx_binary_sequence_ = 0;
};

#endif  // XIAOLING_CONN_WEBSOCKET_H
