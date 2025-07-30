/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "board.h"

#define TAG "MCP"

#define DEFAULT_TOOLCALL_STACK_SIZE 6144

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

    auto display = board.GetDisplay();
    if (display && !display->GetTheme().empty()) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                display->SetTheme(properties["theme"].value<std::string>().c_str());
                return true;
            });

        AddTool("self.show_image",
            "Show an image on the screen. Use this tool if you want to show something to the user.\n"
            "Args:\n"
            "  `url`: The URL of the image to show.",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                FetchNetworkImage(url);
                return true;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                if (!camera->Capture()) {
                    return "{\"success\": false, \"message\": \"Failed to capture photo\"}";
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s", tool->name().c_str());
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
        }
        GetToolsList(id_int, cursor_str);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        auto stack_size = cJSON_GetObjectItem(params, "stackSize");
        if (stack_size != nullptr && !cJSON_IsNumber(stack_size)) {
            ESP_LOGE(TAG, "tools/call: Invalid stackSize");
            ReplyError(id_int, "Invalid stackSize");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments, stack_size ? stack_size->valueint : DEFAULT_TOOLCALL_STACK_SIZE);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments, int stack_size) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Start a task to receive data with stack size
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "tool_call";
    cfg.stack_size = stack_size;
    cfg.prio = 1;
    esp_pthread_set_cfg(&cfg);

    // Use a thread to call the tool to avoid blocking the main thread
    tool_call_thread_ = std::thread([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
    tool_call_thread_.detach();
}

void McpServer::FetchNetworkImage(const std::string &url) {
    int ret;

    ESP_LOGI(TAG, "Fetching network image from %s", url.c_str());

    try {
        auto network = Board::GetInstance().GetNetwork();
        if (!network) {
            ESP_LOGE(TAG, "Network not available");
            return;
        }

        auto http = network->CreateHttp();
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            return;
        }

        http->SetTimeout(15000);
        http->SetHeader("User-Agent", "XiaoZhi-ESP32/1.0");

        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "Failed to connect to %s", url.c_str());
            return;
        }

        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "HTTP status code: %d", status_code);

        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP error: %d", status_code);
            http->Close();
            return;
        }

        std::string image_data = http->ReadAll();
        http->Close();

        if (image_data.empty()) {
            ESP_LOGE(TAG, "No image data received");
            return;
        }

        if (image_data.size() > 2 * 1024 * 1024) {  // 2MB limit
            ESP_LOGE(TAG, "Image too large: %u bytes", image_data.size());
            return;
        }

        ESP_LOGI(TAG, "Downloaded image data: %u bytes", image_data.size());

        if (image_data.empty() || image_data.size() < 10 ||
            image_data[0] != 0xFF || image_data[1] != 0xD8) {
            ESP_LOGE(TAG, "Invalid JPEG header");
            return;
        }

        jpeg_dec_config_t config = {
            .output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
            .rotate = JPEG_ROTATE_0D,
        };

        jpeg_dec_handle_t jpeg_dec = NULL;
        ret = jpeg_dec_open(&config, &jpeg_dec);
        if (ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open JPEG decoder, ret: %d", ret);
            return;
        }

        jpeg_dec_io_t* jpeg_io = (jpeg_dec_io_t*)heap_caps_malloc(sizeof(jpeg_dec_io_t), MALLOC_CAP_SPIRAM);
        if (!jpeg_io) {
            ESP_LOGE(TAG, "Failed to allocate memory for JPEG IO");
            jpeg_dec_close(jpeg_dec);
            return;
        }
        memset(jpeg_io, 0, sizeof(jpeg_dec_io_t));

        jpeg_dec_header_info_t* out_info = (jpeg_dec_header_info_t*)heap_caps_aligned_alloc(16, sizeof(jpeg_dec_header_info_t), MALLOC_CAP_SPIRAM);
        if (!out_info) {
            ESP_LOGE(TAG, "Failed to allocate memory for JPEG output header");
            heap_caps_free(jpeg_io);
            jpeg_dec_close(jpeg_dec);
            return;
        }
        memset(out_info, 0, sizeof(jpeg_dec_header_info_t));

        jpeg_io->inbuf = (uint8_t*)image_data.data();
        jpeg_io->inbuf_len = image_data.size();

        ret = jpeg_dec_parse_header(jpeg_dec, jpeg_io, out_info);
        if (ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to parse JPEG header, ret: %d", ret);
            heap_caps_free(out_info);
            heap_caps_free(jpeg_io);
            jpeg_dec_close(jpeg_dec);
            return;
        }

        ESP_LOGI(TAG, "JPEG info: %dx%d pixels", out_info->width, out_info->height);

        if (out_info->width <= 0 || out_info->height <= 0 || 
            out_info->width > 640 || out_info->height > 480) {
            ESP_LOGE(TAG, "Invalid JPEG dimensions: %dx%d", out_info->width, out_info->height);
            heap_caps_free(out_info);
            heap_caps_free(jpeg_io);
            jpeg_dec_close(jpeg_dec);
            return;
        }

        size_t output_size = out_info->width * out_info->height * 2; // RGB565
        ESP_LOGI(TAG, "Allocating %u bytes for RGB data", output_size);

        uint8_t* rgb_data = (uint8_t*)heap_caps_aligned_alloc(16, output_size, 
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (!rgb_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for RGB data (%u bytes)", output_size);
            heap_caps_free(out_info);
            heap_caps_free(jpeg_io);
            jpeg_dec_close(jpeg_dec);
            return;
        }

        jpeg_io->outbuf = rgb_data;
        int inbuf_consumed = jpeg_io->inbuf_len - jpeg_io->inbuf_remain;
        jpeg_io->inbuf = (uint8_t*)image_data.data() + inbuf_consumed;
        jpeg_io->inbuf_len = jpeg_io->inbuf_remain;

        ESP_LOGI(TAG, "Starting JPEG decode process...");
        ret = jpeg_dec_process(jpeg_dec, jpeg_io);
        if (ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to decode JPEG, ret: %d", ret);
            heap_caps_free(rgb_data);
            heap_caps_free(out_info);
            heap_caps_free(jpeg_io);
            jpeg_dec_close(jpeg_dec);
            return;
        }

        ESP_LOGI(TAG, "JPEG decode successful");

        memset(&network_image_, 0, sizeof(network_image_));
        network_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        network_image_.header.cf = LV_COLOR_FORMAT_RGB565;
        network_image_.header.flags = LV_IMAGE_FLAGS_ALLOCATED | LV_IMAGE_FLAGS_MODIFIABLE;
        network_image_.header.w = out_info->width;
        network_image_.header.h = out_info->height;
        network_image_.header.stride = out_info->width * 2; // RGB565 = 2 bytes per pixel
        network_image_.data_size = out_info->width * out_info->height * 2;
        network_image_.data = rgb_data;

        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->SetPreviewImage(&network_image_);
        } else {
            ESP_LOGE(TAG, "Display not available");
        }

        // heap_caps_free(rgb_data);
        heap_caps_free(out_info);
        heap_caps_free(jpeg_io);
        jpeg_dec_close(jpeg_dec);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in FetchNetworkImage: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "Unknown exception in FetchNetworkImage");
    }
}