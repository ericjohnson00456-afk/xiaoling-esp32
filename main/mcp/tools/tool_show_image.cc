#include "board.h"
#include "display.h"
#include "image_fetcher.h"
#include "mcp_server.h"
#include <esp_log.h>
namespace {
constexpr const char *TAG = "MCP";
MCP_REGISTER_TOOL(RegisterShowImage, McpToolGroup::kCommon, 70) {
#ifdef CONFIG_LSPLATFORM
  auto &board = Board::GetInstance();
  auto *display = board.GetDisplay();
  if (!display)
    return;
  server.AddTool(
      "ls.built_in.show_image", "显示图片\n",
      PropertyList({Property("url", kPropertyTypeString, {"图片资源地址"})}),
      [&board, display](const PropertyList &properties) -> ReturnValue {
        const auto url = properties["url"].value<std::string>();
        auto image = board.GetImageFetcher()->Fetch(url);
        if (!image) {
          ESP_LOGE(TAG, "Failed to fetch image: %s", url.c_str());
          return false;
        }
        display->ShowImage(std::move(image));
        return std::string("已完成操作");
      });
#endif
}
} // namespace
