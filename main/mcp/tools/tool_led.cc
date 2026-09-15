#include <stdexcept>
#include <string>

#include "board.h"
#include "mcp_server.h"

namespace {
MCP_REGISTER_TOOL(RegisterLedTools, McpToolGroup::kCommon, 120) {
  auto *led = Board::GetInstance().GetLed();
  if (led == nullptr) return;
  server.AddTool("ls.led_switch", "控制 LED 的打开或关闭状态。",
                 PropertyList({Property("value", kPropertyTypeBoolean,
                                         {"打开为 true，关闭为 false"})}),
                 [led](const PropertyList &properties) -> ReturnValue {
                   if (properties["value"].value<bool>()) led->TurnOn();
                   else led->TurnOff();
                   return std::string("已完成操作");
                 });
  server.AddTool("ls.led_blink", "控制 LED 闪烁模式。",
                 PropertyList({Property("mode", kPropertyTypeString,
                                         {"off、normal、fast 或 slow"})}),
                 [led](const PropertyList &properties) -> ReturnValue {
                   const auto mode = properties["mode"].value<std::string>();
                   if (mode == "off") led->TurnOff();
                   else if (mode == "fast") led->Blink(200);
                   else if (mode == "normal") led->Blink(500);
                   else if (mode == "slow") led->Blink(1000);
                   else throw std::runtime_error("mode 必须是 off/normal/fast/slow");
                   return std::string("已完成操作");
                 });
}
} // namespace
