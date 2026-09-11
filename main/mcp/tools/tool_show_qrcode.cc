#include "board.h"
#include "display.h"
#include "image_fetcher.h"
#include "mcp_server.h"
#include <esp_log.h>
namespace {
constexpr const char *TAG = "MCP";
MCP_REGISTER_TOOL(RegisterShowQrcode, McpToolGroup::kCommon, 60) {
#ifdef CONFIG_LSPLATFORM
  auto &board = Board::GetInstance();
  auto *display = board.GetDisplay();
  if (!display)
    return;
  server.AddTool(
      "ls.built_in.show_qrcode", "显示二维码\n",
      PropertyList({Property("url", kPropertyTypeString, {"二维码地址"}),
                    Property("message", kPropertyTypeString, "", {"文本提示"}),
                    Property("err_code", kPropertyTypeString, "", {"错误码"})}),
      [&board, display](const PropertyList &properties) -> ReturnValue {
        const auto url = properties["url"].value<std::string>();
        auto image = board.GetImageFetcher()->Fetch(url);
        if (!image) {
          ESP_LOGE(TAG, "Failed to fetch image: %s", url.c_str());
          return false;
        }
        display->ShowActivation(std::move(image),
                                properties["message"].value<std::string>());
        return std::string("已完成操作");
      });
#endif
}
} // namespace
