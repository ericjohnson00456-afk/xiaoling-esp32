#include <algorithm>
#include <stdexcept>
#include <string>

#include "audio_codec.h"
#include "board.h"
#include "mcp_server.h"
#include "shell/cmd/kv/kv_user.h"

namespace {
MCP_REGISTER_TOOL(RegisterSetVolume, McpToolGroup::kCommon, 10) {
  auto &board = Board::GetInstance();
  auto *codec = board.GetAudioCodec();
  if (codec == nullptr) return;
  server.AddTool(
      "ls.set_volume",
      "用于调节设备音量，支持绝对值、相对调节、静音和最大/最小音量。",
      PropertyList({
          Property("intent", kPropertyTypeString, {"set、adjustUp 或 adjustDown"}),
          Property("value", kPropertyTypeString, "", {"数值、max、min 或 mid；没有明确数值时留空"}),
          Property("unit", kPropertyTypeString, "", {"档、级或百分比；没有单位时留空"})}),
      [codec](const PropertyList &properties) -> ReturnValue {
        const auto intent = properties["intent"].value<std::string>();
        const auto value = properties["value"].value<std::string>();
        const auto unit = properties["unit"].value<std::string>();
        int target = codec->output_volume();
        if (intent == "set") {
          if (value == "max") target = 100;
          else if (value == "min" || value == "静音") target = 0;
          else if (value == "mid") target = 50;
          else if (!value.empty()) target = (unit == "档" || unit == "级") ? std::stoi(value) * 10 : std::stoi(value);
          else throw std::runtime_error("音量 value 参数缺失");
        } else if (intent == "adjustUp" || intent == "adjustDown") {
          int step = value.empty() ? 10 : std::stoi(value) * 10;
          if (unit == "百分比" && !value.empty()) step = std::stoi(value);
          target += intent == "adjustUp" ? step : -step;
        } else {
          throw std::runtime_error("intent 不支持，必须是 set/adjustUp/adjustDown");
        }
        codec->SetOutputVolume(
            std::clamp(target, static_cast<int>(KvUser::GetMinVolume()), 100));
        return std::string("已设置音量为") + std::to_string(codec->output_volume()) + "%";
      });
}
} // namespace
