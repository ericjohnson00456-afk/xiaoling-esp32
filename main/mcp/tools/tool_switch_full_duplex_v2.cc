#include <stdexcept>
#include <string>

#include "application.h"
#include "mcp_server.h"

namespace {
MCP_REGISTER_TOOL(RegisterSwitchFullDuplexV2, McpToolGroup::kCommon, 160) {
  server.AddTool(
      "ls.built_in.switch_full_duplex_v2",
      "切换交互模式：全双工可打断、半双工不可打断或单工模式。",
      PropertyList({
          Property("mode", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","description":"Interaction mode value.","enum":["single","duplex","duplex_no_interrupt"]})"},
                   true)}),
      [](const PropertyList &properties) -> ReturnValue {
        const auto raw = properties["mode"].value<std::string>();
        cJSON *mode_json = cJSON_Parse(raw.c_str());
        if (!cJSON_IsString(mode_json) || mode_json->valuestring == nullptr) {
          cJSON_Delete(mode_json);
          throw std::runtime_error("mode 必须是字符串");
        }
        const std::string mode = mode_json->valuestring;
        cJSON_Delete(mode_json);

        auto &app = Application::GetInstance();
        DuplexMode duplex_mode;
        const char *mode_name = nullptr;
        if (mode == "single") {
          duplex_mode = DuplexMode::kSimplex;
          mode_name = "单工";
        } else if (mode == "duplex_no_interrupt") {
          duplex_mode = DuplexMode::kHalfDuplex;
          mode_name = "半双工不可打断";
        } else if (mode == "duplex") {
          if (!app.GetAudioService().SupportsAec()) {
            throw std::runtime_error("当前设备不支持全双工模式");
          }
          duplex_mode = DuplexMode::kFullDuplex;
          mode_name = "全双工可打断";
        } else {
          throw std::runtime_error("不支持的交互模式: " + mode);
        }

        app.SetDuplexMode(duplex_mode);
        return std::string("已切换为") + mode_name;
      });
}
} // namespace
