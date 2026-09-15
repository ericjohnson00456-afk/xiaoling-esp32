#include <algorithm>
#include <stdexcept>
#include <string>

#include "board.h"
#include "mcp_server.h"

namespace {
MCP_REGISTER_TOOL(RegisterDisplaySetBrightness, McpToolGroup::kCommon, 40) {
  auto *backlight = Board::GetInstance().GetBacklight();
  if (backlight == nullptr) return;
  server.AddTool(
      "ls.display_set_brightness",
      "用于调节设备屏幕亮度，支持绝对百分比、相对调亮或调暗，以及最亮、最暗。",
      PropertyList({
          Property("intent", kPropertyTypeString, {"set、adjustUp 或 adjustDown"}),
          Property("value", kPropertyTypeString, "", {"绝对值、调整档位或 max/min；没有明确数值时留空"}),
          Property("unit", kPropertyTypeString, "", {"档、级或百分比；没有单位时留空"})}),
      [backlight](const PropertyList &properties) -> ReturnValue {
        const auto intent = properties["intent"].value<std::string>();
        const auto value = properties["value"].value<std::string>();
        const auto unit = properties["unit"].value<std::string>();
        int current = backlight->brightness();
        int target = current;
        if (intent == "set") {
          if (value == "max") target = 100;
          else if (value == "min") target = 0;
          else if (!value.empty()) target = (unit == "档" || unit == "级") ? std::stoi(value) * 10 : std::stoi(value);
          else throw std::runtime_error("亮度 value 参数缺失");
        } else if (intent == "adjustUp" || intent == "adjustDown") {
          int step = value.empty() ? 10 : std::stoi(value) * 10;
          if (unit == "百分比" && !value.empty()) step = std::stoi(value);
          target = current + (intent == "adjustUp" ? step : -step);
        } else {
          throw std::runtime_error("intent 不支持，必须是 set/adjustUp/adjustDown");
        }
        backlight->SetBrightness(static_cast<uint8_t>(std::clamp(target, 0, 100)), true);
        return std::string("已完成操作");
      });
}
} // namespace
