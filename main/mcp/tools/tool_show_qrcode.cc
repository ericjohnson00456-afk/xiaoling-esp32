#include <stdexcept>
#include <string>

#include "board.h"
#include "image_fetcher.h"
#include "mcp_server.h"
#include "ui/events/ui_events.h"
#include "ui/models/qrcode/qrcode_model.h"
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
        QrCodeModel::GetInstance().SetCustomQrCode(std::move(image), std::move(message));
        const auto err = UiEvents::PostQrCodeRequested();
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Failed to request QR code page: %s", esp_err_to_name(err));
          throw std::runtime_error("二维码页面打开失败");
        }
        return std::string("Operation completed");
      });
#endif
}
} // namespace
