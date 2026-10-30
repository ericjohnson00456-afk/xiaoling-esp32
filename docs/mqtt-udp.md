# 小聆通信方式说明

## 当前实现

当前固件的小聆实时业务统一使用 **HTTP + WebSocket**：

- HTTP：获取访问令牌，以及闹钟、音乐 URL、激活等辅助接口；
- WebSocket：传输语音会话控制 JSON、MCP 消息和二进制音频；
- 上行音频：`opus-wb`，每帧为 2 字节大端长度加 Opus 数据。

连接主机和 WebSocket URL 由 `main/xiaoling/conn/xiaoling_config.h` 生成，默认生产主机为 `api.listenai.com`。

## MQTT/UDP 状态

本仓库当前的 `main/xiaoling/conn` 没有 MQTT 客户端、MQTT topic、UDP 音频 socket 或 AES-CTR 音频通道实现。因此旧版“MQTT 控制 + UDP 音频”的流程、端口、topic、密钥和示例不适用于当前小聆固件，也不应据此进行服务端对接。

如果未来新增 MQTT 或 UDP 传输，需要同时在连接层、鉴权流程、音频会话和本文档中补充实现后再发布协议。

## 相关文档

- [小聆 WebSocket 协议](websocket.md)
- [小聆 MCP 协议](mcp-protocol.md)
- [系统架构](architecture.md)
