#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

#include "application.h"
#include "mcp_server.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
MCP_REGISTER_TOOL(RegisterDeviceControl, McpToolGroup::kCommon, 90) {
  server.AddTool(
      "ls.built_in.device_control", "设备控制，当前支持重启。",
      PropertyList({
          Property("command", kPropertyTypeString, {"目前只支持 reboot"}),
          Property("delay_ms", kPropertyTypeInteger, 0, 0, INT32_MAX, {"重启前等待时间（毫秒）"})}),
      [](const PropertyList &properties) -> ReturnValue {
        const auto command = properties["command"].value<std::string>();
        const auto delay = properties["delay_ms"].value<int>();
        if (command != "reboot") throw std::runtime_error("仅支持 reboot 指令");
        if (delay > 0) {
          std::thread([delay]() {
            vTaskDelay(pdMS_TO_TICKS(delay));
            Application::GetInstance().Reboot();
          }).detach();
          return std::string("设备将在") + std::to_string(delay) + "毫秒后重启";
        }
        Application::GetInstance().Reboot();
        return std::string("设备即将重启");
      });
}
} // namespace
