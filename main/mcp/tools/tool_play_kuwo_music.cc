#include <stdexcept>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "mcp_server.h"
#include <cJSON.h>

namespace {
constexpr const char *kItemsSchema = R"({"type":"object","properties":{"items":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"playable":{"type":"integer"},"itemid":{"type":"string"}},"required":["itemid"]}}},"required":["items"]})";

struct KuwoResolveContext {
  std::vector<std::string> item_ids;
};

void ResolveKuwoTask(void *arg) {
  std::unique_ptr<KuwoResolveContext> context(static_cast<KuwoResolveContext *>(arg));
  std::vector<std::string> urls;
  urls.reserve(context->item_ids.size());
  for (const auto &item_id : context->item_ids) {
    std::string url;
    if (Application::GetInstance().ResolveKuwoMusicUrl(item_id, url)) {
      urls.emplace_back(std::move(url));
    }
  }
  if (!urls.empty()) {
    Application::GetInstance().Schedule([urls = std::move(urls)]() mutable {
      if (Application::GetInstance().GetAudioPlayer().PlayMusicList(urls) == 0) {
        ESP_LOGW("PlayKuwo", "酷我歌曲 URL 已解析，但提交播放失败");
      }
    });
  } else {
    ESP_LOGW("PlayKuwo", "没有解析到可播放的酷我歌曲");
  }
  vTaskDelete(nullptr);
}

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
                   auto context = std::make_unique<KuwoResolveContext>();
                   for (int i = 0; i < cJSON_GetArraySize(items); ++i) {
                     cJSON *item = cJSON_GetArrayItem(items, i);
                     cJSON *id = cJSON_IsObject(item) ? cJSON_GetObjectItem(item, "itemid") : nullptr;
                     if (!cJSON_IsString(id) || id->valuestring == nullptr || id->valuestring[0] == '\0') continue;
                     context->item_ids.emplace_back(id->valuestring);
                   }
                   cJSON_Delete(root);
                   if (context->item_ids.empty()) throw std::runtime_error("没有可播放的酷我音乐");
                   if (xTaskCreate(ResolveKuwoTask, "kuwo_resolve", 8192,
                                   context.release(), 4, nullptr) != pdPASS) {
                     throw std::runtime_error("创建酷我解析任务失败");
                   }
                   return std::string("已提交播放请求");
                 });
}
} // namespace
