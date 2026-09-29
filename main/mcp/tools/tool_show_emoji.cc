#include <stdexcept>
#include <string>

#include "mcp_server.h"
#include "ui/ui.h"

namespace {
MCP_REGISTER_TOOL(RegisterShowEmoji, McpToolGroup::kCommon, 130) {
  std::string names_description = "表情名称";
  const auto names = Ui::GetEmotionNames();
  if (!names.empty()) {
    names_description = "表情类型，可以是 ";
    for (size_t i = 0; i < names.size(); ++i) {
      if (i != 0) names_description += "、";
      names_description += "'" + names[i] + "'";
    }
  }
  server.AddTool("set_emotion", "显示一次指定表情。",
                 PropertyList({Property("emotion", kPropertyTypeString,
                                         {names_description})}),
                 [](const PropertyList &properties) -> ReturnValue {
                   const auto emotion = properties["emotion"].value<std::string>();
                   if (emotion.empty()) throw std::runtime_error("emotion 不能为空");
                   Ui::ShowEmotionOnce(emotion);
                   return std::string("已完成操作");
                 });
}
} // namespace
