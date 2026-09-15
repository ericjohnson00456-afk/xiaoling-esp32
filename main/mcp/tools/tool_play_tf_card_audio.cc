#include <stdexcept>

#include "mcp_server.h"

namespace {
MCP_REGISTER_TOOL(RegisterPlayTfCardAudio, McpToolGroup::kCommon, 22) {
  server.AddTool(
      "ls.built_in.play_tf_card_audio", "播放本地TF卡音频",
      PropertyList({
          Property("mode", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","description":"播放模式，single 表示从指定列表的 start_index 开始播放","enum":["single"]})"},
                   true),
          Property("items", kPropertyTypeJson,
                   JsonSchema{R"({"type":"array","description":"TF卡音频文件列表，端侧按顺序尝试播放","items":{"type":"object","properties":{"file_id":{"type":"string","description":"云端生成的音频文件唯一标识"},"file_name":{"type":"string","description":"音频文件名"},"file_path":{"type":"string","description":"端侧TF卡中的文件路径或稳定文件标识"},"display_name":{"type":"string","description":"用于界面展示或播报的音频名称"}},"required":["file_id","file_name","file_path"]}})"},
                   true),
          Property("start_index", kPropertyTypeJson,
                   JsonSchema{R"({"type":"integer","description":"起播位置，从 items 中的第几个文件开始播放，默认从 0 开始","minimum":0})"},
                   true)}),
      [](const PropertyList &) -> ReturnValue {
        throw std::runtime_error("ls.built_in.play_tf_card_audio 尚未实现");
      });
}
}
