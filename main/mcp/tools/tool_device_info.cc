#include <cstdio>
#include <stdexcept>
#include <string>

#include "board.h"
#include "codecs/audio_codec.h"
#include "mcp_server.h"
#include "system/system_info.h"
#include <esp_app_desc.h>
#include <cJSON.h>

namespace {
MCP_REGISTER_TOOL(RegisterDeviceInfo, McpToolGroup::kCommon, 100) {
  server.AddTool(
      "ls.get_device_info",
      "查询设备当前状态，可查询音量、屏幕亮度、固件版本、电量和预计待机时间。",
      PropertyList({Property("fields", kPropertyTypeJson, JsonSchema{R"({"type":"array","items":{"type":"string","enum":["volume","brightness","version","battery","standby_time"]}})"}, true,
          {"需要查询的字段数组"})}),
      [](const PropertyList &properties) -> ReturnValue {
        const auto raw = properties["fields"].value<std::string>();
        cJSON *fields = cJSON_Parse(raw.c_str());
        if (fields == nullptr || !cJSON_IsArray(fields)) {
          cJSON_Delete(fields);
          throw std::runtime_error("fields 必须是数组");
        }
        bool volume = false, brightness = false, version = false, battery = false, standby = false;
        cJSON *field = nullptr;
        cJSON_ArrayForEach(field, fields) {
          if (!cJSON_IsString(field) || field->valuestring == nullptr) {
            cJSON_Delete(fields);
            throw std::runtime_error("fields 中存在非字符串元素");
          }
          const std::string name = field->valuestring;
          if (name == "volume") volume = true;
          else if (name == "brightness") brightness = true;
          else if (name == "version") version = true;
          else if (name == "battery") battery = true;
          else if (name == "standby_time") standby = true;
          else {
            cJSON_Delete(fields);
            throw std::runtime_error("fields 包含不支持的字段: " + name);
          }
        }
        cJSON_Delete(fields);
        if (!volume && !brightness && !version && !battery && !standby)
          throw std::runtime_error("fields 不能为空");

        cJSON *payload = cJSON_CreateObject();
        if (payload == nullptr) throw std::runtime_error("创建设备状态失败");
        auto &board = Board::GetInstance();
        if (volume && board.GetAudioCodec()) cJSON_AddNumberToObject(payload, "volume", board.GetAudioCodec()->output_volume());
        if (brightness) cJSON_AddNumberToObject(payload, "brightness", board.GetBacklight() ? board.GetBacklight()->brightness() : 0);
        if (version) cJSON_AddStringToObject(payload, "version", esp_app_get_description()->version);
        if (battery || standby) {
          int level = 0; bool charging = false; bool discharging = false;
          if (!board.GetBatteryLevel(level, charging, discharging)) {
            cJSON_Delete(payload);
            throw std::runtime_error("获取电量失败");
          }
          cJSON_AddNumberToObject(payload, "battery", level);
          const int minutes = level * (6 * 60) / 100;
          char text[64];
          snprintf(text, sizeof(text), "大概是%d小时%d分钟", minutes / 60, minutes % 60);
          cJSON_AddStringToObject(payload, "standby_time", text);
        }
        return payload;
      });
}
} // namespace


