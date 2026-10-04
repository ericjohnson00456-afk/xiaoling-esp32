#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "application.h"
#include "mcp_server.h"
#include "player/types/music_track.h"
#include <cJSON.h>

namespace {
constexpr const char *kItemsSchema =
    R"({"type":"object","properties":{"items":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"playable":{"type":"integer"},"itemid":{"oneOf":[{"type":"string"},{"type":"integer"}]}},"required":["itemid"]}}},"required":["items"]})";

std::string ParseDisplayName(const cJSON *name) {
  if (!cJSON_IsString(name) || name->valuestring == nullptr) {
    return {};
  }
  const std::string raw_name = name->valuestring;
  cJSON *parsed = cJSON_ParseWithLength(raw_name.data(), raw_name.size());
  if (parsed == nullptr) {
    return raw_name;
  }

  std::string display_name;
  const cJSON *value = parsed;
  if (cJSON_IsArray(parsed)) {
    value = cJSON_GetArrayItem(parsed, 0);
  }
  if (cJSON_IsString(value) && value->valuestring != nullptr) {
    display_name = value->valuestring;
  }
  cJSON_Delete(parsed);
  return display_name.empty() ? raw_name : display_name;
}

MCP_REGISTER_TOOL(RegisterPlayKuwoMusic, McpToolGroup::kCommon, 20) {
  server.AddTool(
      "ls.built_in.play_kuwo_music", "播放酷我音乐。",
      PropertyList(
          {Property("result", kPropertyTypeJson, JsonSchema{kItemsSchema}, true,
                    {"包含歌曲 items 数组，也兼容直接数组"})}),
      [](const PropertyList &properties) -> ReturnValue {
        const auto raw = properties["result"].value<std::string>();
        cJSON *root = cJSON_Parse(raw.c_str());
        if (root == nullptr)
          throw std::runtime_error("result JSON 格式错误");
        cJSON *items =
            cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "items");
        if (cJSON_IsObject(root)) {
          cJSON *nested = cJSON_GetObjectItem(root, "result");
          if (cJSON_IsObject(nested))
            items = cJSON_GetObjectItem(nested, "items");
        }
        if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0) {
          cJSON_Delete(root);
          throw std::runtime_error("没有可播放的酷我音乐");
        }
        std::vector<MusicTrack> tracks;
        for (int i = 0; i < cJSON_GetArraySize(items); ++i) {
          cJSON *item = cJSON_GetArrayItem(items, i);
          cJSON *id = cJSON_IsObject(item) ? cJSON_GetObjectItem(item, "itemid")
                                           : nullptr;
          std::string item_id;
          if (cJSON_IsString(id) && id->valuestring != nullptr &&
              id->valuestring[0] != '\0') {
            item_id = id->valuestring;
          } else if (cJSON_IsNumber(id) && std::isfinite(id->valuedouble) &&
                     id->valuedouble >= 0 &&
                     std::floor(id->valuedouble) == id->valuedouble) {
            char text[32] = {};
            std::snprintf(text, sizeof(text), "%.0f", id->valuedouble);
            item_id = text;
          }
          if (!item_id.empty()) {
            cJSON *name = cJSON_IsObject(item)
                              ? cJSON_GetObjectItem(item, "name")
                              : nullptr;
            tracks.push_back(MusicTrack{
                .id = std::move(item_id),
                .name = ParseDisplayName(name),
                .source = MusicSource::kKuwoOnline,
            });
          }
        }
        cJSON_Delete(root);
        if (tracks.empty())
          throw std::runtime_error("没有可播放的酷我音乐");
        if (!Application::GetInstance()
                 .GetIntentManager()
                 .Music()
                 .SubmitKuwoList(std::move(tracks)))
          throw std::runtime_error("提交酷我播放列表失败");
        return std::string("播放成功");
      });
}
} // namespace
