#include <stdexcept>

#include "mcp_server.h"

namespace {
MCP_REGISTER_TOOL(RegisterSwitchFullDuplexV2, McpToolGroup::kCommon, 160) {
  server.AddTool(
      "ls.built_in.switch_full_duplex_v2",
      "该工具用于切换交互模式：全双工可打断、全双工不可打断、单工模式",
      PropertyList({
          Property("mode", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","description":"Interaction mode value.","enum":["single","duplex","duplex_no_interrupt"]})"},
                   true)}),
      [](const PropertyList &) -> ReturnValue {
        throw std::runtime_error("ls.built_in.switch_full_duplex_v2 尚未实现");
      });
}
}
