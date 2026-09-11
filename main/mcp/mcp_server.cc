/* MCP 服务端：协议分发与工具调度。具体工具实现位于 mcp/tools 目录。 */

#include "mcp_server.h"

#include <algorithm>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_log.h>

#include "application.h"
#include "board.h"

namespace {
constexpr const char *TAG = "MCP";

McpToolRegistrar*& ToolRegistrarHead() {
  static McpToolRegistrar* head = nullptr;
  return head;
}

void RegisterToolGroup(McpServer& server, McpToolGroup group) {
  std::vector<McpToolRegistrar*> registrations;
  for (auto* registration = ToolRegistrarHead(); registration != nullptr;
       registration = registration->next()) {
    if (registration->group() == group && registration->callback() != nullptr) {
      registrations.push_back(registration);
    }
  }

  std::sort(registrations.begin(), registrations.end(),
            [](const McpToolRegistrar* left, const McpToolRegistrar* right) {
              return left->order() < right->order();
            });

  for (auto* registration : registrations) {
    registration->callback()(server);
  }
}

std::string JsonString(const cJSON *item) {
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return item->valuestring;
  }
  if (cJSON_IsNumber(item)) {
    return std::to_string(item->valueint);
  }
  return {};
}
} // namespace

McpToolRegistrar::McpToolRegistrar(McpToolGroup group, uint8_t order,
                                   McpToolRegister callback)
    : group_(group), order_(order), callback_(callback) {
  next_ = ToolRegistrarHead();
  ToolRegistrarHead() = this;
}

McpServer::McpServer() = default;

McpServer::~McpServer() {
  for (auto *tool : tools_) {
    delete tool;
  }
}

void McpServer::AddCommonTools() {
  // 通用工具需排在板级工具前，以便云端复用前缀提示缓存。
  auto original_tools = std::move(tools_);
  RegisterToolGroup(*this, McpToolGroup::kCommon);
  tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
  RegisterToolGroup(*this, McpToolGroup::kUserOnly);
}

void McpServer::AddTool(McpTool *tool) {
  if (std::find_if(tools_.begin(), tools_.end(),
                   [tool](const McpTool *current) {
                     return current->name() == tool->name();
                   }) != tools_.end()) {
    ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
    delete tool;
    return;
  }
  ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(),
           tool->user_only() ? " [user]" : "");
  tools_.push_back(tool);
}

void McpServer::AddTool(
    const std::string &name, const std::string &description,
    const PropertyList &properties,
    std::function<ReturnValue(const PropertyList &)> callback) {
  AddTool(new McpTool(name, description, properties, std::move(callback)));
}

void McpServer::AddUserOnlyTool(
    const std::string &name, const std::string &description,
    const PropertyList &properties,
    std::function<ReturnValue(const PropertyList &)> callback) {
  auto *tool = new McpTool(name, description, properties, std::move(callback));
  tool->set_user_only(true);
  AddTool(tool);
}

void McpServer::ParseMessage(const std::string &message) {
  cJSON *json = cJSON_Parse(message.c_str());
  if (json == nullptr) {
    ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
    return;
  }
  ParseMessage(json);
  cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON *capabilities) {
  const auto *vision = cJSON_GetObjectItem(capabilities, "vision");
  if (!cJSON_IsObject(vision)) {
    return;
  }
  const auto *url = cJSON_GetObjectItem(vision, "url");
  if (!cJSON_IsString(url)) {
    return;
  }
  auto *camera = Board::GetInstance().GetCamera();
  if (camera == nullptr) {
    return;
  }
  const auto *token = cJSON_GetObjectItem(vision, "token");
  camera->SetExplainUrl(url->valuestring,
                        cJSON_IsString(token) ? token->valuestring : "");
}

void McpServer::ParseMessage(const cJSON *json) {
  const auto *method_item = cJSON_GetObjectItem(json, "method");
  if (!cJSON_IsString(method_item)) {
    ESP_LOGE(TAG, "Missing MCP method");
    return;
  }
  const std::string method = method_item->valuestring;
  if (method.rfind("notifications", 0) == 0) {
    return;
  }

  // 小聆 MCP 载荷没有 jsonrpc 字段；若字段存在，仍校验标准版本。
  const auto *version = cJSON_GetObjectItem(json, "jsonrpc");
  if (version != nullptr &&
      (!cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0)) {
    ESP_LOGE(TAG, "Invalid JSONRPC version");
    return;
  }
  const auto *params = cJSON_GetObjectItem(json, "params");
  if (params != nullptr && !cJSON_IsObject(params)) {
    ESP_LOGE(TAG, "Invalid params for method: %s", method.c_str());
    return;
  }
  const std::string id = JsonString(cJSON_GetObjectItem(json, "id"));
  if (id.empty()) {
    ESP_LOGE(TAG, "Invalid id for method: %s", method.c_str());
    return;
  }

  if (method == "initialize") {
    if (cJSON_IsObject(params)) {
      const auto *capabilities = cJSON_GetObjectItem(params, "capabilities");
      if (cJSON_IsObject(capabilities)) {
        ParseCapabilities(capabilities);
      }
    }
    const auto *app_desc = esp_app_get_description();
    std::string result =
        "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
    result += app_desc->version;
    result += "\"}}";
    ReplyResult(method, id, result);
  } else if (method == "tools/list") {
    std::string cursor;
    bool with_user_tools = false;
    if (cJSON_IsObject(params)) {
      cursor = JsonString(cJSON_GetObjectItem(params, "cursor"));
      const auto *value = cJSON_GetObjectItem(params, "withUserTools");
      with_user_tools = cJSON_IsBool(value) && value->valueint != 0;
    }
    GetToolsList(id, cursor, with_user_tools);
  } else if (method == "tools/call") {
    if (!cJSON_IsObject(params)) {
      ReplyError(method, id, "Missing params");
      return;
    }
    const auto *name = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name)) {
      ReplyError(method, id, "Missing name");
      return;
    }
    const auto *arguments = cJSON_GetObjectItem(params, "arguments");
    if (arguments != nullptr && !cJSON_IsObject(arguments)) {
      ReplyError(method, id, "Invalid arguments");
      return;
    }
    DoToolCall(id, name->valuestring, arguments);
  } else {
    ReplyError(method, id, "Method not implemented: " + method);
  }
}

void McpServer::ReplyResult(const std::string &method, const std::string &id,
                            const std::string &result) {
  Application::GetInstance().SendMcpMessage("{\"method\":\"" + method +
                                            "\",\"id\":\"" + id +
                                            "\",\"result\":" + result + "}");
}

void McpServer::ReplyError(const std::string &method, const std::string &id,
                           const std::string &message) {
  Application::GetInstance().SendMcpMessage(
      "{\"method\":\"" + method + "\",\"id\":\"" + id +
      "\",\"error\":{\"message\":\"" + message + "\"}}");
}

void McpServer::GetToolsList(const std::string &id, const std::string &cursor,
                             bool list_user_only_tools) {
  constexpr size_t kMaxPayloadSize = 8000;
  std::string result = "{\"tools\":[";
  bool found_cursor = cursor.empty();
  std::string next_cursor;
  for (auto *tool : tools_) {
    if (!found_cursor) {
      found_cursor = tool->name() == cursor;
      if (!found_cursor)
        continue;
    }
    if (!list_user_only_tools && tool->user_only())
      continue;
    const auto tool_json = tool->to_json();
    if (result.size() + tool_json.size() + 32 > kMaxPayloadSize) {
      next_cursor = tool->name();
      break;
    }
    if (result.back() != '[')
      result += ',';
    result += tool_json;
  }
  if (result.back() == '[' && !tools_.empty() && !next_cursor.empty()) {
    ReplyError("tools/list", id,
               "Tool exceeds payload size limit: " + next_cursor);
    return;
  }
  result += ']';
  if (!next_cursor.empty())
    result += ",\"nextCursor\":\"" + next_cursor + "\"";
  result += '}';
  ReplyResult("tools/list", id, result);
}

void McpServer::DoToolCall(const std::string &id, const std::string &tool_name,
                           const cJSON *tool_arguments) {
  const auto tool = std::find_if(tools_.begin(), tools_.end(),
                                 [&tool_name](const McpTool *value) {
                                   return value->name() == tool_name;
                                 });
  if (tool == tools_.end()) {
    ReplyError("tools/call", id, "Unknown tool: " + tool_name);
    return;
  }
  PropertyList arguments = (*tool)->properties();
  try {
    for (auto &argument : arguments) {
      bool found = false;
      const auto *value =
          cJSON_IsObject(tool_arguments)
              ? cJSON_GetObjectItem(tool_arguments, argument.name().c_str())
              : nullptr;
      if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
        argument.set_value<bool>(value->valueint != 0);
        found = true;
      } else if (argument.type() == kPropertyTypeInteger &&
                 cJSON_IsNumber(value)) {
        argument.set_value<int>(value->valueint);
        found = true;
      } else if (argument.type() == kPropertyTypeString &&
                 cJSON_IsString(value)) {
        argument.set_value<std::string>(value->valuestring);
        found = true;
      }
      if (!argument.has_default_value() && !found) {
        ReplyError("tools/call", id,
                   "Missing valid argument: " + argument.name());
        return;
      }
    }
  } catch (const std::exception &error) {
    ReplyError("tools/call", id, error.what());
    return;
  }
  auto &app = Application::GetInstance();
  app.Schedule([this, id, tool, arguments = std::move(arguments)]() {
    try {
      ReplyResult("tools/call", id, (*tool)->Call(arguments));
    } catch (const std::exception &error) {
      ESP_LOGE(TAG, "tools/call: %s", error.what());
      ReplyError("tools/call", id, error.what());
    }
  });
}
