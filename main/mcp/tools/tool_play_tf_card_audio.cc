#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>

#include "application.h"
#include "mcp_server.h"
#include "player/types/music_track.h"
#include "ui/ui.h"

namespace {

constexpr char kTag[] = "PlayTfCardAudio";
constexpr char kMountPoint[] = "/sdcard";
constexpr size_t kMaxPathBytes = 512;

bool EndsWithMp3(const std::string& value) {
  if (value.size() < 4) return false;
  const size_t offset = value.size() - 4;
  return value[offset] == '.' &&
         std::tolower(static_cast<unsigned char>(value[offset + 1])) == 'm' &&
         std::tolower(static_cast<unsigned char>(value[offset + 2])) == 'p' &&
         value[offset + 3] == '3';
}

bool HasParentTraversal(const std::string& path) {
  size_t begin = 0;
  while (begin <= path.size()) {
    const size_t end = path.find('/', begin);
    const size_t length = (end == std::string::npos ? path.size() : end) - begin;
    if (length == 2 && path.compare(begin, length, "..") == 0) return true;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return false;
}

std::string NormalizeSdPath(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  if (path.rfind("/SD:/", 0) == 0) {
    path.replace(0, 5, "/sdcard/");
  } else if (path.rfind("SD:/", 0) == 0) {
    path.replace(0, 4, "/sdcard/");
  } else if (path.rfind("sdcard/", 0) == 0) {
    path.insert(path.begin(), '/');
  }
  return path;
}

std::string BuildFilePath(const std::string& raw_path,
                          const std::string& file_name) {
  std::string path = NormalizeSdPath(raw_path);
  if (path.empty() || file_name.empty() || HasParentTraversal(path) ||
      HasParentTraversal(file_name)) {
    throw std::runtime_error("TF卡音频文件路径无效");
  }

  const bool already_has_name =
      path.size() >= file_name.size() &&
      path.compare(path.size() - file_name.size(), file_name.size(), file_name) == 0;
  if (!already_has_name && !EndsWithMp3(path)) {
    if (path.back() != '/') path.push_back('/');
    path += file_name;
  }
  if ((path != kMountPoint && path.rfind(std::string(kMountPoint) + "/", 0) != 0) ||
      path.size() >= kMaxPathBytes || !EndsWithMp3(path)) {
    throw std::runtime_error("TF卡音频文件路径无效");
  }
  return path;
}

std::string WithoutMp3Suffix(std::string name) {
  if (EndsWithMp3(name)) name.resize(name.size() - 4);
  return name;
}

std::string RequiredString(const cJSON* item, const char* key) {
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(item, key);
  if (!cJSON_IsString(value) || value->valuestring == nullptr ||
      value->valuestring[0] == '\0') {
    throw std::runtime_error(std::string("items 中缺少 ") + key);
  }
  return value->valuestring;
}

void ShowFileNotFound(const std::string& file_id, const std::string& path) {
  ESP_LOGW(kTag, "TF 卡音频文件不可用: id=%s, path=%s",
           file_id.c_str(), path.c_str());
  Ui::ShowToast("TF卡音频播放失败", 3000);
  throw std::runtime_error("未找到对应的TF卡音频文件");
}

MCP_REGISTER_TOOL(RegisterPlayTfCardAudio, McpToolGroup::kCommon, 22) {
  server.AddTool(
      "ls.built_in.play_tf_card_audio", "播放本地TF卡音频",
      PropertyList({
          Property("mode", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","description":"播放模式，single 表示从指定列表的 start_index 开始播放","enum":["single"]})"},
                   true),
          Property("items", kPropertyTypeJson,
                   JsonSchema{R"({"type":"array","description":"TF卡音频文件列表，端侧按顺序尝试播放","items":{"type":"object","properties":{"file_id":{"type":"string","description":"云端生成的音频文件唯一标识"},"file_name":{"type":"string","description":"音频文件名"},"file_path":{"type":"string","description":"端侧TF卡中的文件路径或目录"},"display_name":{"type":"string","description":"用于界面展示的音频名称"}},"required":["file_id","file_name","file_path"]}})"},
                   true),
          Property("start_index", kPropertyTypeJson,
                   JsonSchema{R"({"type":"integer","description":"起播位置，越界时从 0 开始","minimum":0})"},
                   true)}),
      [](const PropertyList& properties) -> ReturnValue {
        const std::string mode_raw = properties["mode"].value<std::string>();
        cJSON* mode = cJSON_Parse(mode_raw.c_str());
        const bool valid_mode = cJSON_IsString(mode) && mode->valuestring != nullptr &&
                                std::string(mode->valuestring) == "single";
        cJSON_Delete(mode);
        if (!valid_mode) {
          throw std::runtime_error("mode 参数无效，当前仅支持 single");
        }

        const std::string items_raw = properties["items"].value<std::string>();
        cJSON* items = cJSON_Parse(items_raw.c_str());
        if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0) {
          cJSON_Delete(items);
          throw std::runtime_error("items 参数无效或为空");
        }

        int start_index = 0;
        const std::string start_raw =
            properties["start_index"].value<std::string>();
        cJSON* start = cJSON_Parse(start_raw.c_str());
        if (cJSON_IsNumber(start)) start_index = start->valueint;
        cJSON_Delete(start);
        const int item_count = cJSON_GetArraySize(items);
        if (start_index < 0 || start_index >= item_count) start_index = 0;

        std::vector<MusicTrack> tracks;
        tracks.reserve(item_count);
        try {
          for (int i = 0; i < item_count; ++i) {
            const cJSON* item = cJSON_GetArrayItem(items, i);
            if (!cJSON_IsObject(item)) {
              throw std::runtime_error("items 中存在无效曲目");
            }
            std::string file_id = RequiredString(item, "file_id");
            std::string file_name = RequiredString(item, "file_name");
            std::string path =
                BuildFilePath(RequiredString(item, "file_path"), file_name);
            struct stat info = {};
            if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
              ShowFileNotFound(file_id, path);
            }
            const cJSON* display =
                cJSON_GetObjectItemCaseSensitive(item, "display_name");
            std::string display_name =
                cJSON_IsString(display) && display->valuestring != nullptr &&
                        display->valuestring[0] != '\0'
                    ? display->valuestring
                    : WithoutMp3Suffix(file_name);
            ESP_LOGI(kTag, "TF 卡曲目[%d]: id=%s, name=%s, path=%s", i,
                     file_id.c_str(), display_name.c_str(), path.c_str());
            tracks.push_back(MusicTrack{
                .id = std::move(file_id),
                .location = std::move(path),
                .name = std::move(display_name),
                .source = MusicSource::kTfCard,
            });
          }
        } catch (...) {
          cJSON_Delete(items);
          throw;
        }
        cJSON_Delete(items);

        const std::string played_file_id =
            tracks[static_cast<std::size_t>(start_index)].id;
        if (!Application::GetInstance()
                 .GetIntentManager()
                 .Music()
                 .SubmitTfCardList(std::move(tracks),
                                   static_cast<std::size_t>(start_index))) {
          throw std::runtime_error("提交TF卡播放列表失败");
        }
        cJSON* result = cJSON_CreateObject();
        if (result == nullptr) return std::string("已开始播放");
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddStringToObject(result, "played_file_id", played_file_id.c_str());
        cJSON_AddStringToObject(result, "message", "started");
        return result;
      });
}

}  // namespace
