#include <stdexcept>
#include <string>

#include "board.h"
#include "image_fetcher.h"
#include "mcp_server.h"
#include "ui/ui.h"
#include <esp_log.h>

namespace {
constexpr const char *TAG = "MCP";
MCP_REGISTER_TOOL(RegisterShowQrcode, McpToolGroup::kCommon, 60) {
#ifdef CONFIG_LSPLATFORM
  auto &board = Board::GetInstance();
  auto *fetcher = board.GetImageFetcher();
  if (fetcher == nullptr) return;
  server.AddTool(
      "ls.built_in.show_qrcode", "显示二维码。",
      PropertyList({Property("url", kPropertyTypeString, {"二维码图片地址"}),
                    Property("message", kPropertyTypeString, "", {"文本提示"}),
                    Property("err_code", kPropertyTypeString, "", {"错误码"})}),
      [fetcher](const PropertyList &properties) -> ReturnValue {
        const auto url = properties["url"].value<std::string>();
        auto image = fetcher->Fetch(url);
        if (!image) {
          ESP_LOGE(TAG, "Failed to fetch QR image: %s", url.c_str());
          throw std::runtime_error("二维码下载失败");
        }
        auto message = properties["message"].value<std::string>();
        const auto err_code = properties["err_code"].value<std::string>();
        if (!err_code.empty()) message += "\n" + err_code;
        Ui::ShowQrCode(std::move(image), std::move(message));
        return std::string("Operation completed");
      });
#endif
}
} // namespace
