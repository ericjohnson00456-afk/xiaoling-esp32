# 小聆 MCP 使用说明

小聆 MCP 让云端通过 WebSocket 调用 ESP32 设备上的工具。工具由固件启动时注册，通用工具位于 `main/mcp/tools/`，板级工具由 `Board::RegisterMcpTools()` 注册。

## 调用示例

请求：

```json
{"action":"mcp","method":"tools/call","params":{"name":"ls.built_in.show_loading","arguments":{}},"id":"100"}
```

成功响应：

```json
{"action":"mcp","method":"tools/call","id":"100","result":{"content":[{"type":"text","text":"ok"}],"isError":false}}
```

完整的初始化、分页获取工具和错误格式见 [mcp-protocol.md](mcp-protocol.md)。

## 固件中注册工具

同步工具示例：

```cpp
MCP_REGISTER_TOOL(RegisterMyTool, McpToolGroup::kCommon, 200) {
  server.AddTool(
      "self.example.set_value",
      "设置一个示例值",
      PropertyList({Property("value", kPropertyTypeInteger, 0, 100)}),
      [](const PropertyList& properties) -> ReturnValue {
        return ReturnValue("ok");
      });
}
```

异步工具使用 `AddAsyncTool`，在业务完成或失败时调用回调。工具名称应保持稳定，避免与已有名称重复。

## 调试要点

- 确认设备已收到 `action: connected`，否则 MCP 消息不会发送。
- `id` 可为字符串或数字，但设备回复时会按字符串回传。
- `tools/list` 可能分页，必须处理 `nextCursor`。
- `tools/call` 的参数必须是对象；参数类型需与工具声明一致。
- 设备日志会打印 `MCP` 注册、解析和调用错误。
