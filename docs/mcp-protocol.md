# 小聆 MCP 协议

小聆 MCP 用于云端调用设备能力。它通过小聆 WebSocket 的文本帧传输，顶层使用 `action: "mcp"`，MCP 请求内容直接放在同一 JSON 对象中。

## 消息封装

```json
{
  "action": "mcp",
  "method": "tools/call",
  "params": {
    "name": "ls.built_in.show_loading",
    "arguments": {}
  },
  "id": "42"
}
```

固件解析 `method`、`params` 和 `id`。如果存在 `jsonrpc`，必须为 `"2.0"`；当前实现允许省略该字段。设备回复同样使用 `action: "mcp"`：

```json
{
  "action": "mcp",
  "method": "tools/call",
  "id": "42",
  "result": {"content":[{"type":"text","text":"ok"}],"isError":false}
}
```

错误响应：

```json
{"action":"mcp","method":"tools/call","id":"42","error":{"message":"Unknown tool: xxx"}}
```

## 支持的方法

### initialize

云端可发送初始化请求。设备会处理 `params.capabilities.vision`，其中的 `url` 和可选 `token` 会配置摄像头识图服务，并返回固件版本：

```json
{
  "action":"mcp",
  "method":"initialize",
  "params":{"capabilities":{"vision":{"url":"https://example/vision","token":"..."}}},
  "id":"1"
}
```

返回的 `result` 包含 `protocolVersion: "2024-11-05"`、`serverInfo.name`（板型名称）和 `serverInfo.version`（固件版本）。

### tools/list

```json
{"action":"mcp","method":"tools/list","params":{},"id":"2"}
```

返回 `result.tools`。单次返回限制约 8000 字节，工具较多时返回 `nextCursor`，下一次请求将其作为 `params.cursor`。传入 `params.withUserTools=true` 才会包含用户专属工具。

工具来自两部分：`main/mcp/tools/` 中的通用工具，以及当前开发板 `Board::RegisterMcpTools()` 注册的板级工具。因此工具列表随板型和编译功能变化，不能在协议文档中写死。

### tools/call

```json
{
  "action":"mcp",
  "method":"tools/call",
  "params":{"name":"ls.built_in.show_loading","arguments":{}},
  "id":"3"
}
```

参数类型由工具描述决定，目前支持布尔、整数、字符串和 JSON 值；缺少没有默认值的参数会返回错误。异步工具在完成或失败时只回复一次。

## 传输与时序

1. 小聆 WebSocket 完成鉴权并收到 `action: connected`。
2. 云端发送 MCP 请求，设备在 Application 任务中执行工具。
3. 设备通过同一 WebSocket 返回 `action: mcp` 响应。
4. `notifications/*` 方法会被设备忽略；未实现的方法返回错误。

MCP 分发代码位于 `main/mcp/mcp_server.cc`，封装代码位于 `main/xiaoling/conn/xiaoling_conn_message.cc`。不要将旧文档中的 `payload`、`jsonrpc` 必填或 MQTT/UDP 传输假设用于当前小聆链路。
