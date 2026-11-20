#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "sdkconfig.h"
#if CONFIG_MINIAPP
#include "miniapp/miniapp.h"
#include "miniapp/miniapp_install.h"
#include "mcp_server.h"
#include <cmath>
#include <cstring>
#include <set>
#include <cstdio>
#include <memory>

namespace {
bool CollectArgumentKeys(const cJSON *args, std::set<std::string> &keys, std::string &error) {
    if (args && !cJSON_IsObject(args)) {
        error = "arguments must be an object";
        return false;
    }
    for (auto *item = args ? args->child : nullptr; item; item = item->next) {
        if (!item->string || !keys.insert(item->string).second) {
            error = "duplicate argument";
            return false;
        }
    }
    return true;
}

bool ValidateInstallArguments(const cJSON *args, std::string &error) {
    std::set<std::string> keys;
    if (!CollectArgumentKeys(args, keys, error))
        return false;
    if (keys != std::set<std::string>{"id", "version", "name", "url", "size", "hash"}) {
        error = "expected id, version, name, url, size and hash";
        return false;
    }
    for (const auto *key : {"id", "version", "name", "url", "hash"}) {
        const auto *value = cJSON_GetObjectItemCaseSensitive(args, key);
        size_t limit = !strcmp(key, "url") ? 2047 : (!strcmp(key, "hash") ? 32 : 127);
        if (!cJSON_IsString(value) || !value->valuestring[0] ||
            strlen(value->valuestring) > limit) {
            error = "invalid string argument";
            return false;
        }
    }
    const auto *size = cJSON_GetObjectItemCaseSensitive(args, "size");
    if (!cJSON_IsNumber(size) || !std::isfinite(size->valuedouble) || size->valuedouble < 1 ||
        size->valuedouble > MINIAPP_SOURCE_MAX ||
        floor(size->valuedouble) != size->valuedouble) {
        error = "invalid source size";
        return false;
    }
    const char *hash = cJSON_GetObjectItemCaseSensitive(args, "hash")->valuestring;
    if (strlen(hash) != 32 || strspn(hash, "0123456789abcdef") != 32) {
        error = "invalid MD5";
        return false;
    }
    const char *url = cJSON_GetObjectItemCaseSensitive(args, "url")->valuestring;
    if ((strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) ||
        strpbrk(url, " \t\r\n")) {
        error = "invalid source URL";
        return false;
    }
    return true;
}

bool ValidateActionArguments(const cJSON *args, std::string &error) {
    std::set<std::string> keys;
    if (!CollectArgumentKeys(args, keys, error))
        return false;
    if (keys != std::set<std::string>{"action", "arguments"} ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(args, "arguments")) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(args, "action"))) {
        error = "expected action and arguments object";
        return false;
    }
    auto *action = cJSON_GetObjectItemCaseSensitive(args, "action");
    if (!*action->valuestring || strlen(action->valuestring) > 63) {
        error = "invalid action name";
        return false;
    }
    return true;
}

bool ValidateEmptyArguments(const cJSON *args, std::string &error) {
    if (args && (!cJSON_IsObject(args) || args->child)) {
        error = "expected empty arguments";
        return false;
    }
    return true;
}

void ControlWorker(void *value) {
    using Work = std::function<void()>;
    std::unique_ptr<Work> work(static_cast<Work *>(value));
    (*work)();
    work.reset();
    vTaskDelete(nullptr);
}
void Queue(std::function<void()> work, McpToolSuccessCallback failure) {
    auto task = std::make_unique<std::function<void()>>(std::move(work));
    if (xTaskCreate(ControlWorker, "miniapp_mcp", 4096, task.get(), 4, nullptr) == pdPASS)
        task.release();
    else
        failure(McpTextResult{"miniapp worker unavailable", true});
}
MCP_REGISTER_TOOL(RegisterMiniapp, McpToolGroup::kCommon, 110) {
    server.AddAsyncTool(
        "ls.built_in.miniapp_install", "下载、验证并启动 Lua 小应用，失败时保留当前应用。",
        PropertyList({Property("id", kPropertyTypeString), Property("version", kPropertyTypeString),
                      Property("name", kPropertyTypeString), Property("url", kPropertyTypeString),
                      Property("size", kPropertyTypeInteger, 1, MINIAPP_SOURCE_MAX),
                      Property("hash", kPropertyTypeString)}),
        [](const PropertyList &args, McpToolSuccessCallback success, McpToolErrorCallback) {
            miniapp_package_t package{};
            snprintf(package.id, sizeof(package.id), "%s", args["id"].value<std::string>().c_str());
            snprintf(package.version, sizeof(package.version), "%s",
                     args["version"].value<std::string>().c_str());
            snprintf(package.name, sizeof(package.name), "%s",
                     args["name"].value<std::string>().c_str());
            snprintf(package.hash, sizeof(package.hash), "%s",
                     args["hash"].value<std::string>().c_str());
            package.size = args["size"].value<int>();
            Miniapp::InstallUrl(package, args["url"].value<std::string>(),
                                [success](bool ok, std::string text) {
                                    success(McpTextResult{std::move(text), !ok});
                                });
        }, ValidateInstallArguments);
    server.AddTool("ls.built_in.miniapp_exit", "退出小应用并返回桌面。", PropertyList(),
                   [](const PropertyList &) -> ReturnValue {
                       if (!miniapp_is_active())
                           return std::string("no miniapp is running");
                       const bool ok = miniapp_exit() == 0;
                       return McpTextResult{ok ? "miniapp exit requested" : "miniapp queue is busy",
                                            !ok};
                   }, ValidateEmptyArguments);
    server.AddAsyncTool(
        "ls.built_in.miniapp_describe", "查询前台小应用的身份、状态和可用操作。", PropertyList(),
        [](const PropertyList &, McpToolSuccessCallback success, McpToolErrorCallback) {
            if (!miniapp_is_active()) {
                success(McpTextResult{R"({"running":false})", false});
                return;
            }
            Queue(
                [success]() {
                    char error[128]{};
                    cJSON *result = miniapp_describe(error, sizeof(error));
                    success(McpTextResult{result ? Miniapp::JsonText(result) : error, !result});
                },
                success);
        }, ValidateEmptyArguments);
    server.AddAsyncTool(
        "ls.built_in.miniapp_action", "根据 miniapp_describe 的声明执行操作。",
        PropertyList(
            {Property("action", kPropertyTypeString),
             Property("arguments", kPropertyTypeJson, JsonSchema{R"({"type":"object"})"}, true)}),
        [](const PropertyList &args, McpToolSuccessCallback success, McpToolErrorCallback) {
            auto action = args["action"].value<std::string>();
            auto json = args["arguments"].value<std::string>();
            Queue(
                [success, action, json]() {
                    cJSON *arguments = cJSON_Parse(json.c_str());
                    char error[128]{};
                    cJSON *result = miniapp_action(action.c_str(), arguments, error, sizeof(error));
                    cJSON_Delete(arguments);
                    if (!result) {
                        success(McpTextResult{error, true});
                        return;
                    }
                    auto *ok = cJSON_GetObjectItemCaseSensitive(result, "ok");
                    auto *message = cJSON_GetObjectItemCaseSensitive(result, "message");
                    success(McpTextResult{cJSON_IsString(message) ? message->valuestring
                                                                  : "invalid action result",
                                          !cJSON_IsTrue(ok)});
                    cJSON_Delete(result);
                },
                success);
        }, ValidateActionArguments);
}
} // namespace
#endif
