#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include <esp_pthread.h>
namespace {
MCP_REGISTER_TOOL(RegisterTakePhoto, McpToolGroup::kCommon, 80) {
  auto *camera = Board::GetInstance().GetCamera();
  if (!camera)
    return;
#ifdef CONFIG_LSPLATFORM
  server.AddTool("ls.built_in.take_photo", "拍照\n", PropertyList(),
                 [camera](const PropertyList &) -> ReturnValue {
                   TaskPriorityReset priority_reset(1);
                   if (!camera->Capture())
                     throw std::runtime_error("Failed to capture photo");
                   cJSON *json = cJSON_Parse(camera->Explain("").c_str());
                   if (!json)
                     throw std::runtime_error("解析原始JSON失败");
                   const auto *data = cJSON_GetObjectItem(json, "data");
                   const auto *url = cJSON_IsObject(data)
                                         ? cJSON_GetObjectItem(data, "url")
                                         : nullptr;
                   if (!cJSON_IsString(url)) {
                     cJSON_Delete(json);
                     throw std::runtime_error("未找到有效的url字段");
                   }
                   ImageUrlContent result(url->valuestring);
                   cJSON_Delete(json);
                   return result;
                 });
#endif
}
} // namespace
