#include <stdexcept>
#include <string>

#include "board.h"
#include "display/display.h"
#include "image_fetcher.h"
#include "mcp_server.h"
#include "ui/screens/home/home_presenter.h"
#include <esp_log.h>

namespace {
constexpr const char *TAG = "MCP";
MCP_REGISTER_TOOL(RegisterShowImage, McpToolGroup::kCommon, 70) {
#ifdef CONFIG_LSPLATFORM
  auto &board = Board::GetInstance();
  auto *fetcher = board.GetImageFetcher();
  if (fetcher == nullptr) return;
  server.AddTool("ls.built_in.show_image", "显示网络图片，参数为图片 URL。",
                 PropertyList({Property("url", kPropertyTypeString, {"http/https 图片地址"})}),
                 [fetcher](const PropertyList &properties) -> ReturnValue {
                   const auto url = properties["url"].value<std::string>();
                   if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
                     throw std::runtime_error("图片 URL 必须使用 http 或 https");
                   auto image = fetcher->Fetch(url);
                   if (!image) {
                     ESP_LOGE(TAG, "Failed to fetch image: %s", url.c_str());
                     throw std::runtime_error("图片下载失败");
                   }
                   HomePresenter::GetInstance().SetImage(std::move(image));
                   return std::string("已完成操作");
                 });
#endif
}
} // namespace
