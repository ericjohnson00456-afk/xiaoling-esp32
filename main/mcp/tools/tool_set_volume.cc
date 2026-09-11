#include <algorithm>

#include "audio_codec.h"
#include "board.h"
#include "mcp_server.h"

namespace {
MCP_REGISTER_TOOL(RegisterSetVolume, McpToolGroup::kCommon, 10) {
  auto &board = Board::GetInstance();
#ifdef CONFIG_LSPLATFORM
  server.AddTool(
      "ls.set_volume",
      "音量控制与调节工具。当用户发出调节音量、控制声音大小的指令时调用。支持绝"
      "对值设置（如：调到50%"
      "）、相对值调节（如：大声点、调小2档）以及极值控制（如：静音、最大声）。",
      PropertyList(
          {Property("text", kPropertyTypeString,
                    {"用户输入的关于音量调节的意图"}),
           Property("intent", kPropertyTypeString,
                    {"调节类型：set（设定到具体值或极值，如：调到10档、最大声、"
                     "最小声）；adjustUp（调大声、加两档）；adjustDown（调小声"
                     "、小一点）"}),
           Property(
               "value", kPropertyTypeString, "",
               {"具体的数值或状态：1. 数字（如：1, 10, 50）；2. "
                "标准状态位：max（最大声、满格）、min（最小声、静音）、mid（中"
                "等音量），如果没有明确参数，则留空，如（大点儿声音）"}),
           Property("unit", kPropertyTypeString, "",
                    {"描述数值的单位。若指令中提及'档'、'级'、'%'"
                     "则对应填写，否则留空。"})}),
      [&board](const PropertyList &properties) -> ReturnValue {
        const auto intent = properties["intent"].value<std::string>();
        const auto value = properties["value"].value<std::string>();
        const auto unit = properties["unit"].value<std::string>();
        auto *codec = board.GetAudioCodec();
        int target = codec->output_volume();
        if (intent == "set") {
          if (value == "max")
            target = 100;
          else if (value == "min" || value == "静音")
            target = 0;
          else if (value == "mid")
            target = 50;
          else if (!value.empty())
            target = unit == "档" || unit == "级"
                         ? std::clamp(std::stoi(value) * 10, 0, 100)
                         : std::clamp(std::stoi(value), 0, 100);
          else
            throw std::runtime_error("Invalid value");
        } else if (intent == "adjustUp" || intent == "adjustDown") {
          const int step = value.empty() ? 10 : std::stoi(value) * 10;
          target = std::clamp(target + (intent == "adjustUp" ? step : -step), 0,
                              100);
        } else
          throw std::runtime_error("Invalid intent");
        codec->SetOutputVolume(target);
        return std::string("已完成操作");
      });
#endif
}
} // namespace
