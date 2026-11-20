#include "miniapp.h"
#include "miniapp_input.h"
#include "miniapp_storage.h"
#include "miniapp_store.h"
#include "miniapp_synth.h"
#include "miniapp_audio.h"
#include "board.h"
#include "mcp_server.h"
#include "display/lvgl_display.h"
#include "codecs/audio_codec.h"
#include <esp_app_desc.h>
#include <cJSON.h>

namespace {
bool HasScreen() {
    auto *display = Board::GetInstance().GetDisplay();
    return dynamic_cast<LvglDisplay *>(display) && display->width() > 0 && display->height() >= 16;
}
bool HasSpeaker() {
    auto *codec = Board::GetInstance().GetAudioCodec();
    return codec && codec->output_sample_rate() > 0;
}
} // namespace
extern "C" cJSON *miniapp_capabilities() {
    auto *root = cJSON_Parse(
        R"({"schema_version":2,"firmware_info":{"type":"esp32"},"runtime":{},"hardware":{"network":true,"display":{},"input":{},"audio":{},"lighting":{"leds":[]}}})");
    if (!root)
        return nullptr;
    auto *firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware_info");
    cJSON_AddStringToObject(firmware, "version", esp_app_get_description()->version);
    auto *hardware = cJSON_GetObjectItemCaseSensitive(root, "hardware");
    auto *display = cJSON_GetObjectItemCaseSensitive(hardware, "display");
    cJSON_AddBoolToObject(display, "supported", HasScreen());
    if (HasScreen()) {
        cJSON_AddNumberToObject(display, "width_px", Board::GetInstance().GetDisplay()->width());
        cJSON_AddNumberToObject(display, "height_px", Board::GetInstance().GetDisplay()->height());
        cJSON_AddStringToObject(display, "shape", Board::GetInstance().GetDisplayShape());
    }
#if CONFIG_MINIAPP
    cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(hardware, "input"), "buttons",
                          MiniappButtonsJson());
#else
    cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(hardware, "input"), "buttons",
                          cJSON_CreateArray());
#endif
    auto *audio = cJSON_GetObjectItemCaseSensitive(hardware, "audio");
    auto *codec = Board::GetInstance().GetAudioCodec();
    cJSON_AddBoolToObject(audio, "speaker", HasSpeaker());
    cJSON_AddBoolToObject(audio, "microphone", codec && codec->input_sample_rate() > 0);
#if CONFIG_MINIAPP
    if (miniapp_has_led()) {
        auto *leds = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(hardware, "lighting"), "leds");
        cJSON_AddItemToArray(leds,
                             cJSON_Parse(R"({"id":"status","label":"状态灯","color":false})"));
    }
    auto *sdk = cJSON_Parse(
        R"({"storage":{"max_bytes":1024,"max_apps":4,"default_ttl_seconds":604800,"max_ttl_seconds":2592000,"write_interval_ms":10000},"http":{"methods":["GET","POST"],"max_pending":4,"max_url_bytes":511,"max_headers":16,"max_headers_bytes":1024,"max_body_bytes":8192,"max_response_bytes":32768,"default_response_bytes":8192,"max_timeout_ms":60000,"default_timeout_ms":10000}})");
    if (!sdk) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddNumberToObject(sdk, "version", MINIAPP_API_VERSION);
    cJSON_AddNumberToObject(sdk, "source_max_bytes", MINIAPP_SOURCE_MAX);
    cJSON_AddBoolToObject(sdk, "retained_source", miniapp_store_supported());
    cJSON_AddNumberToObject(sdk, "heap_max_bytes", MINIAPP_LUA_HEAP_LIMIT);
    if (HasScreen())
        cJSON_AddItemToObject(
            sdk, "graphics",
            cJSON_Parse(R"({"max_shapes":256,"max_texts":16,"text_max_bytes":63})"));
    if (HasSpeaker()) {
        cJSON_AddItemToObject(
            sdk, "tts",
            cJSON_Parse(R"({"text_max_bytes":512,"max_pending":1,"timeout_ms":120000})"));
        cJSON_AddItemToObject(
            sdk, "audio",
            cJSON_Parse(R"({"max_url_bytes":511,"max_players":1,"live":true,"state_query":true})"));
        cJSON_AddItemToObject(
            sdk, "synth",
            cJSON_Parse(
                R"({"sample_rate":16000,"max_voices":4,"max_notes":256,"max_duration_ms":60000,"queue_depth":4,"waveforms":["square","triangle","noise"],"loop":true,"pause_resume":true})"));
    }
    cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(root, "runtime"), "lua_sdk", sdk);
#endif
    return root;
}

namespace {
MCP_REGISTER_TOOL(RegisterDeviceCapabilities, McpToolGroup::kCommon, 109) {
    server.AddTool("ls.built_in.get_device_capabilities", "查询实际板型硬件和 Lua 小应用能力。",
                   PropertyList(),
                   [](const PropertyList &) -> ReturnValue { return miniapp_capabilities(); },
                   [](const cJSON *args, std::string &error) {
                       if (args && (!cJSON_IsObject(args) || args->child)) {
                           error = "expected empty arguments";
                           return false;
                       }
                       return true;
                   });
}
} // namespace
