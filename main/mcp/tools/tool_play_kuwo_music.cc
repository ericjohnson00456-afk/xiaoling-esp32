#include <stdexcept>
#include <string>

#include "application.h"
#include "mcp_server.h"
#include <cJSON.h>

namespace {
constexpr const char *kItemsSchema = R"({"type":"object","properties":{"items":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"playable":{"type":"integer"},"itemid":{"type":"string"},"url":{"type":"string"}},"required":["itemid"]}}},"required":["items"]})";

MCP_REGISTER_TOOL(RegisterPlayKuwoMusic, McpToolGroup::kCommon, 20) {
  server.AddTool("ls.built_in.play_kuwo_music", "播放酷我音乐。",
                 PropertyList({Property("result", kPropertyTypeJson, JsonSchema{kItemsSchema}, true,
                                         {"包含歌曲 items 数组，也兼容直接数组"})}),
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
                     cJSON_Delete(root); throw std::runtime_error("没有可播放的酷我音乐");
                   }
                   std::string url;
                   for (int i = 0; i < cJSON_GetArraySize(items); ++i) {
                     cJSON *item = cJSON_GetArrayItem(items, i);
                     cJSON *id = cJSON_IsObject(item) ? cJSON_GetObjectItem(item, "itemid") : nullptr;
                     if (!cJSON_IsString(id) || id->valuestring == nullptr || id->valuestring[0] == '\0') continue;
                     cJSON *url_item = cJSON_GetObjectItem(item, "url");
                     if (cJSON_IsString(url_item) && url_item->valuestring) { url = url_item->valuestring; break; }
                   }
                   cJSON_Delete(root);
                   // The current ESP32 protocol has no Kuwo item-id transport.
                   // If the server supplies a resolved URL, use the normal player.
                   if (!url.empty() && Application::GetInstance().GetAudioPlayer().PlayTtsUrl(url) == 0)
                     throw std::runtime_error("提交酷我音乐播放失败");
                   return std::string("已提交播放请求");
                 });
}
} // namespace
