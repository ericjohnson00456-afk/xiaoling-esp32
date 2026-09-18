#include <string>
#include "mcp_server.h"
#include "ui/ui_app.h"

namespace {
MCP_REGISTER_TOOL(RegisterShowLoading, McpToolGroup::kCommon, 131) {
  server.AddTool("ls.built_in.show_loading", "显示等待状态。",
                 PropertyList({Property("text", kPropertyTypeString, "请稍等...", {"等待提示文本"})}),
                 [](const PropertyList &properties) -> ReturnValue {
                   (void)properties;
                   UiApp::ShowEmotionOnce("wait");
                   return std::string("已完成操作");
                 });
}
} // namespace
