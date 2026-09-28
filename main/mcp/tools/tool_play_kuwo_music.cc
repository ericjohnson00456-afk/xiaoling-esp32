#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "application.h"
#include "mcp_server.h"
#include <cJSON.h>

namespace {
constexpr const char *kItemsSchema = R"({"type":"object","properties":{"items":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"playable":{"type":"integer"},"itemid":{"oneOf":[{"type":"string"},{"type":"integer"}]}},"required":["itemid"]}}},"required":["items"]})";

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
                   std::vector<std::string> item_ids;
                   for (int i = 0; i < cJSON_GetArraySize(items); ++i) {
                     cJSON *item = cJSON_GetArrayItem(items, i);
                     cJSON *id = cJSON_IsObject(item) ? cJSON_GetObjectItem(item, "itemid") : nullptr;
                     if (cJSON_IsString(id) && id->valuestring != nullptr && id->valuestring[0] != '\0') {
                       item_ids.emplace_back(id->valuestring);
                     } else if (cJSON_IsNumber(id) && std::isfinite(id->valuedouble) &&
                                id->valuedouble >= 0 && std::floor(id->valuedouble) == id->valuedouble) {
                       char text[32] = {};
                       std::snprintf(text, sizeof(text), "%.0f", id->valuedouble);
                       item_ids.emplace_back(text);
                     }
                   }
                   cJSON_Delete(root);
                   if (item_ids.empty()) throw std::runtime_error("没有可播放的酷我音乐");
                   if (!Application::GetInstance().SubmitKuwoMusicList(std::move(item_ids)))
                     throw std::runtime_error("提交酷我播放列表失败");
                   return std::string("播放成功");
                 });
}
} // namespace
