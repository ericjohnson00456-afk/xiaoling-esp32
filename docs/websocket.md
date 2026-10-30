# 小聆 WebSocket 协议

本文描述当前固件在 `main/xiaoling/conn` 和 `main/xiaoling/session` 中实现的小聆实时链路。协议由小聆平台定义，本文只记录设备端实际发送、接收和处理的字段。

## 连接地址与鉴权

生产环境主机为 `api.listenai.com`；测试环境可在设备设置中切换到 `staging-api.listenai.com` 或 `integration-api.listenai.com`。设备先通过 HTTP 获取访问令牌，再连接：

```text
ws://<host>/v1/interaction?param=<Base64(JSON)>
```

`param` 解码后的 JSON 由固件生成：

```json
{
  "scene": "main",
  "mcp": true,
  "tool_protocol_version": "v2",
  "type": "fullduplex",
  "firmware_info": {"type": "esp32", "version": "2.0.0"}
}
```

WebSocket 请求头：

| Header | 来源 | 作用 |
| --- | --- | --- |
| `Authorization` | 访问令牌 | Bearer 鉴权 |
| `Device-Id` | 云端设备 ID | 标识设备 |
| `Client-Id` | 板级 UUID | 标识固件客户端 |

当前固件使用 `opus-wb` 上行编码：16 kHz、单声道、20 ms Opus 帧。配置为 `opus-wb` 时，每个二进制帧是 **2 字节大端长度 + Opus 数据**。下行二进制帧按 16 kHz 音频包交给播放器解码。

## 文本消息

文本帧是 JSON 对象，顶层以 `action` 区分业务。小聆链路不使用旧版 `type: hello/listen/tts` 文档中的消息格式。

### 建立链路

服务器发送以下消息后，设备才将协议状态标记为已连接：

```json
{"action":"connected"}
```

### 开始语音会话

设备发送 `action=start`，`params` 由会话模式决定。典型全双工语音会话如下：

```json
{
  "action": "start",
  "params": {
    "fullduplex": "1",
    "fullduplex_timeout": "30",
    "data_type": "audio",
    "aue": "opus-wb",
    "features": ["nlu", "tts"],
    "nlu_properties": {
      "sn": "<device-id>",
      "custom": {"rid": "<request-id>"},
      "abilities": [{"name":"event","intents":["reply_interrupted"]}]
    },
    "asr_properties": {"audio_gain":"3.0"}
  }
}
```

服务器返回 `action:started` 后，设备开始按当前会话上传音频。`rid` 是会话关联 ID；旧会话的 `started/result/finish/closed` 消息会被设备丢弃。

文本请求（例如闹钟 TTS、拍照识图）也使用 `action:start`，但 `data_type` 为 `text`，随后发送一帧以 `NUL` 结尾的文本数据。拍照识图会在 `nlu_properties.custom.image` 中携带 Base64 JPEG。

### 会话控制

| action | 方向 | 说明 |
| --- | --- | --- |
| `start` | 设备→云端 | 创建音频或文本会话 |
| `cancel` | 设备→云端 | 取消当前会话 |
| `abort` | 设备→云端 | 上报 TTS 被唤醒词打断，并带原回复游标 |
| `event` | 设备→云端 | 上报 `reply_interrupted` 等事件 |
| `mcp` | 双向 | MCP 请求或响应 |
| `connected` | 云端→设备 | 协议连接就绪 |
| `started` | 云端→设备 | 会话已启动 |
| `result` | 云端→设备 | 返回识别、对话或工具结果 |
| `finish` / `closed` | 云端→设备 | 会话结束 |
| `error` | 云端→设备 | 云端错误；`code=401` 会清除本地令牌 |

服务器返回的 `result` 通常在 `data` 中携带 `sub`、文本、情绪、TTS URL 或字幕信息，具体字段由小聆平台版本决定，设备按 `action`、`rid` 和 `data.sub` 分发。

## MCP

MCP 消息使用顶层 `action: "mcp"`，其余字段直接放在同一个 JSON 对象中；当前实现不会额外包裹 `payload`，也不强制要求 `jsonrpc` 字段。详见 [mcp-protocol.md](mcp-protocol.md)。

设备收到未知 action、非法 JSON 或不属于当前 `rid` 的会话消息时会记录日志并忽略。

## 重连与诊断

WebSocket 传输连接和小聆协议连接分别统计。传输断开后固件自动重连；只有收到新的 `connected` 后，`IsConnected()` 才恢复为真。`start` 发出后约 1 秒未收到 `started` 会按链路异常处理并触发重连。当前连接、重连、DNS/TCP 探测和音频发送耗时可通过串口日志诊断。

## 代码位置

- `main/xiaoling/conn/xiaoling_conn.cc`：连接生命周期、队列和重连
- `main/xiaoling/conn/xiaoling_conn_message.cc`：URL、文本消息和 MCP 封装
- `main/xiaoling/session/session_voice.cc`：语音会话和二进制音频
- `main/xiaoling/conn/xiaoling_conn_websocket.cc`：WebSocket 传输封装
