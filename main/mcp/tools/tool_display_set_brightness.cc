#include "board.h"
#include "mcp_server.h"
namespace {
MCP_REGISTER_TOOL(RegisterDisplaySetBrightness, McpToolGroup::kCommon, 40) {
  auto *backlight = Board::GetInstance().GetBacklight();
  if (!backlight)
    return;
#ifdef CONFIG_LSPLATFORM
  server.AddTool(
      "ls.display_set_brightness",
      "设置显示器的亮度。亮度可以10%为步长进行设置 (0, 10, 20, ..., "
      "100)。当用户无具体设置数值时,得先调用display_get_"
      "brightness工具获取当前亮度，再做调整。\n",
      PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100,
                             {"亮度值，以10%为步长 (0-100)"})}),
      [backlight](const PropertyList &properties) -> ReturnValue {
        backlight->SetBrightness(
            static_cast<uint8_t>(properties["brightness"].value<int>()), true);
        return std::string("已完成操作");
      });
#endif
}
} // namespace
