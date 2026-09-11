#include "audio_codec.h"
#include "board.h"
#include "mcp_server.h"
#include <sstream>
namespace {
MCP_REGISTER_TOOL(RegisterGetVolume, McpToolGroup::kCommon, 20) {
#ifdef CONFIG_LSPLATFORM
  auto &board = Board::GetInstance();
  server.AddTool("ls.get_volume", "获取当前音量。", PropertyList(),
                 [&board](const PropertyList &) -> ReturnValue {
                   std::ostringstream output;
                   output << "当前音量为"
                          << board.GetAudioCodec()->output_volume();
                   return output.str();
                 });
#endif
}
} // namespace
