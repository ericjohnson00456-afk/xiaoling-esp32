#include "mcp_server.h"
namespace {
MCP_REGISTER_TOOL(RegisterPlayKuwoMusic, McpToolGroup::kCommon, 30) {
#ifdef CONFIG_LSPLATFORM
  server.AddTool("ls.built_in.play_kuwo_music", "播放酷我音乐", PropertyList(),
                 [](const PropertyList &) -> ReturnValue {
                   return std::string("已完成操作");
                 });
#endif
}
} // namespace
