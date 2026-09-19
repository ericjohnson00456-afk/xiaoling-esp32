#include <cstdint>
#include <stdexcept>
#include <string>

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
          Property("delay_ms", kPropertyTypeInteger, 0, 0, INT32_MAX, {"重启前等待时间（毫秒，最少 200 毫秒）"})}),
      [](const PropertyList &properties) -> ReturnValue {
        const auto command = properties["command"].value<std::string>();
        const auto delay = properties["delay_ms"].value<int>();
        if (command != "reboot") throw std::runtime_error("仅支持 reboot 指令");
        constexpr int kMinimumRebootDelayMs = 200;
        const auto reboot_delay = delay < kMinimumRebootDelayMs ? kMinimumRebootDelayMs : delay;

        // Queue reboot behind the MCP response task. The extra scheduling
        // layer ensures the response task is queued before reboot. Keep a
        // minimum delay so the response has time to reach the cloud.
        Application::GetInstance().Schedule([reboot_delay]() {
          Application::GetInstance().Schedule([reboot_delay]() {
            vTaskDelay(pdMS_TO_TICKS(reboot_delay));
            Application::GetInstance().Reboot();
          });
        });
        return std::string("已完成操作");
      });
}
} // namespace
