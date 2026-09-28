#include <stdexcept>
#include <string>
#include <vector>

#include "application.h"
#include "mcp_server.h"
#include <cJSON.h>

namespace {
constexpr const char *kItemsSchema = R"({"type":"object","properties":{"items":{"type":"array","items":{"type":"object","properties":{"url":{"type":"string"},"name":{"type":"string"}},"required":["url"]}}},"required":["items"]})";

MCP_REGISTER_TOOL(RegisterPlayAudioUrl, McpToolGroup::kCommon, 21) {
  server.AddTool("ls.built_in.play_audio_link", "播放音频 URL。",
                 PropertyList({Property("result", kPropertyTypeJson, JsonSchema{kItemsSchema}, true,
                                         {"包含 items 数组的结果对象，也兼容直接数组"})}),
                 [](const PropertyList &properties) -> ReturnValue {
                   const auto raw = properties["result"].value<std::string>();
                   cJSON *root = cJSON_Parse(raw.c_str());
                   if (root == nullptr) throw std::runtime_error("result JSON 格式错误");
                   cJSON *items = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "items");
                   if (cJSON_IsObject(root)) {
                     cJSON *nested = cJSON_GetObjectItem(root, "result");
                     if (cJSON_IsObject(nested)) items = cJSON_GetObjectItem(nested, "items");
                   }
                   if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0) {
                     cJSON_Delete(root); throw std::runtime_error("没有可播放的音频");
                   }
                   std::vector<std::string> urls;
                   for (int i = 0; i < cJSON_GetArraySize(items); ++i) {
                     cJSON *item = cJSON_GetArrayItem(items, i);
                     cJSON *url_item = cJSON_IsObject(item) ? cJSON_GetObjectItem(item, "url") : nullptr;
                     if (cJSON_IsString(url_item) && url_item->valuestring && url_item->valuestring[0] != '\0')
                       urls.emplace_back(url_item->valuestring);
                   }
                   cJSON_Delete(root);
                   if (urls.empty())
                     throw std::runtime_error("没有可播放的音频 URL");
                   if (!Application::GetInstance().SubmitMusicUrlList(std::move(urls)))
                     throw std::runtime_error("提交音频播放失败");
                   return std::string("播放成功");
                 });
}
} // namespace

