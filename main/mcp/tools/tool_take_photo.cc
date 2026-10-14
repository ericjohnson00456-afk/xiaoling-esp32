#include "board.h"
#include "camera/camera_service.h"
#include "mcp_server.h"

#include <utility>

namespace {
MCP_REGISTER_TOOL(RegisterTakePhoto, McpToolGroup::kCommon, 80) {
  auto *camera = Board::GetInstance().GetCamera();
  if (!camera)
    return;
#ifdef CONFIG_LSPLATFORM
  server.AddAsyncTool(
      "ls.built_in.take_photo", "进入三秒拍照预览并返回照片 URL。",
      PropertyList({Property("sync", kPropertyTypeBoolean, false,
                             {"同步拍照；为 true 时云端不下发主动播报 TTS URL"})}),
      [](const PropertyList &args, McpToolSuccessCallback success,
         McpToolErrorCallback error) {
        CameraService::GetInstance().StartVoiceMcpCapture(
            args["sync"].value<bool>(),
            [success = std::move(success), error = std::move(error)](
                bool completed, std::string value) mutable {
              if (completed) {
                success(ImageUrlContent(value));
              } else {
                error(std::move(value));
              }
            });
      });
#endif
}
} // namespace
