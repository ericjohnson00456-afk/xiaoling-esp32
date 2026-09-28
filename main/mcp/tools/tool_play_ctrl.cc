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
                             {"RESUME_PLAY、PAUSE、CHOOSE_PREVIOUS、"
                              "CHOOSE_NEXT、REPLAY 或 STOP"})}),
      [](const PropertyList &properties) -> ReturnValue {
        // MCP Server 已将工具调用投递到 Application 主任务，可直接操作播放器。
        const auto intent = properties["intent"].value<std::string>();
        auto &app = Application::GetInstance();
        auto &player = app.GetAudioPlayer();
        auto &music = app.GetIntentManager().Music();

        if (!player.HasMusicList()) {
          throw std::runtime_error("当前没有音乐播放列表");
        }

        bool success = true;
        if (intent == "PAUSE") {
          success = player.PauseMusic();
          if (success) music.SetUserPaused(true);
        } else if (intent == "RESUME_PLAY") {
          music.SetUserPaused(false);
        } else if (intent == "STOP") {
          success = player.PauseMusic();
          if (success) music.SetUserPaused(true);
        } else if (intent == "CHOOSE_PREVIOUS") {
          // false 表示只修改列表索引，不抢在回复 TTS 前播放。
          success = player.PlayPreviousMusic(false);
          if (success) music.SetUserPaused(false);
        } else if (intent == "CHOOSE_NEXT") {
          success = player.PlayNextMusic(false);
          if (success) music.SetUserPaused(false);
        } else if (intent == "REPLAY") {
          success = player.ReplayMusic(false);
          if (success) music.SetUserPaused(false);
        } else {
          throw std::runtime_error("不支持的播放控制指令");
        }
        if (!success) {
          throw std::runtime_error("播放器操作失败");
        }

        music.ReturnToStandby();
        return std::string("执行成功");
      });
}
} // namespace
