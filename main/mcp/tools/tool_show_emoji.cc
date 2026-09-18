#include <stdexcept>
#include <string>

#include "mcp_server.h"
#include "ui/ui_app.h"
#include "ui/ui_manager.h"
#include "display/display.h"
#include "ui/themes/lvgl_theme.h"
#include "ui/utils/emoji/emoji_repository.h"

namespace {
MCP_REGISTER_TOOL(RegisterShowEmoji, McpToolGroup::kCommon, 130) {
  std::string names_description = "表情名称";
  if (auto *display = UiManager::GetInstance().display(); display != nullptr) {
    if (auto *theme = dynamic_cast<LvglTheme *>(display->GetTheme()); theme != nullptr &&
        theme->emoji_repository() != nullptr) {
      const auto names = theme->emoji_repository()->GetNames();
      if (!names.empty()) {
        names_description = "表情类型，可以是 ";
        for (size_t i = 0; i < names.size(); ++i) {
          if (i != 0) names_description += "、";
          names_description += "'" + names[i] + "'";
        }
      }
    }
  }
  server.AddTool("set_emotion", "显示一次指定表情。",
                 PropertyList({Property("emotion", kPropertyTypeString,
                                         {names_description})}),
                 [](const PropertyList &properties) -> ReturnValue {
                   const auto emotion = properties["emotion"].value<std::string>();
                   if (emotion.empty()) throw std::runtime_error("emotion 不能为空");
                   UiApp::ShowEmotionOnce(emotion);
                   return std::string("已完成操作");
                 });
}
} // namespace
