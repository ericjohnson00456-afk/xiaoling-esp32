#include "application.h"
#include "mcp_server.h"
#include <string>

namespace {
MCP_REGISTER_TOOL(RegisterExit, McpToolGroup::kCommon, 132) {
  server.AddTool("ls.built_in.exit",
                 "退出当前对话并停止上行，同时清除音乐播放列表。",
                 PropertyList(), [](const PropertyList &) -> ReturnValue {
                   auto &app = Application::GetInstance();
                   app.GetIntentManager().Music().Exit();
                   app.AbortSpeaking(kAbortReasonExit);
                   return std::string("已完成操作");
                 });
}
} // namespace
