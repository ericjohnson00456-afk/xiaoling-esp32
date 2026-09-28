#include <stdexcept>
#include <string>

#include "application.h"
#include "mcp_server.h"

namespace {
MCP_REGISTER_TOOL(RegisterPlaybackControl, McpToolGroup::kCommon, 23) {
  server.AddTool(
      "ls.playback_control",
      "控制当前播放器：暂停、恢复、上一首、下一首、重播或停止。",
      PropertyList({Property("intent", kPropertyTypeString,
                             {"RESUME_PLAY、PAUSE、CHOOSE_PREVIOUS、CHOOSE_NEXT、REPLAY 或 STOP"})}),
      [](const PropertyList &properties) -> ReturnValue {
        const auto intent = properties["intent"].value<std::string>();
        if (intent == "PAUSE") {
          if (!Application::GetInstance().RequestPauseMusic())
            throw std::runtime_error("当前没有可暂停的播放会话");
        } else if (intent == "RESUME_PLAY") {
          if (!Application::GetInstance().RequestResumeMusic())
            throw std::runtime_error("当前没有可恢复的播放会话");
        } else if (intent == "STOP") {
          if (!Application::GetInstance().RequestStopMusic())
            throw std::runtime_error("当前没有可停止的音乐播放会话");
        } else if (intent == "CHOOSE_PREVIOUS") {
          if (!Application::GetInstance().RequestPreviousMusic())
            throw std::runtime_error("当前没有可切换的播放列表");
        } else if (intent == "CHOOSE_NEXT") {
          if (!Application::GetInstance().RequestNextMusic())
            throw std::runtime_error("当前没有可切换的播放列表");
        } else if (intent == "REPLAY") {
          if (!Application::GetInstance().RequestReplayMusic())
            throw std::runtime_error("当前没有可重播的音乐");
        } else {
          throw std::runtime_error("不支持的播放控制指令");
        }
        return std::string("执行成功");
      });
}
} // namespace
