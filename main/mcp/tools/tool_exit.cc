#include <string>
#include "application.h"
#include "mcp_server.h"

namespace {
MCP_REGISTER_TOOL(RegisterExit, McpToolGroup::kCommon, 132) {
  server.AddTool("ls.built_in.exit", "退出当前对话并停止上行，保留当前播放。", PropertyList(),
                 [](const PropertyList &) -> ReturnValue {
                   Application::GetInstance().AbortSpeaking(kAbortReasonExit);
                   return std::string("已完成操作");
                 });
}
} // namespace
