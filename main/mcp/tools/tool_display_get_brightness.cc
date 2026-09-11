#include "board.h"
#include "mcp_server.h"
#include <sstream>
namespace {
MCP_REGISTER_TOOL(RegisterDisplayGetBrightness, McpToolGroup::kCommon, 50) {
#ifdef CONFIG_LSPLATFORM
  auto *backlight = Board::GetInstance().GetBacklight();
  if (!backlight)
    return;
  server.AddTool("ls.display_get_brightness", "获取显示器的当前亮度。",
                 PropertyList(),
                 [backlight](const PropertyList &) -> ReturnValue {
                   std::ostringstream output;
                   output << "当前显示器亮度为"
                          << static_cast<int>(backlight->brightness());
                   return output.str();
                 });
#endif
}
} // namespace
