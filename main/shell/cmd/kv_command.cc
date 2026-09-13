#include "kv_command.h"

#include "settings.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <esp_console.h>
#include <nvs.h>

namespace {

constexpr size_t kNvsNameMaxLength = 15;

void PrintUsage() {
    std::printf(
        "Usage:\n"
        "  kv show [namespace]\n"
        "  kv clean <namespace>\n"
        "  kv del <namespace> <key>\n"
        "  kv set <string|int|bool> <namespace> <key> <value>\n"
        "  kv get <string|int|bool> <namespace> <key>\n"
        "  kv help\n");
}

bool IsValidNvsName(const char* value) {
    return value != nullptr && value[0] != '\0' && std::strlen(value) <= kNvsNameMaxLength;
}

bool ParseInt32(const char* text, int32_t& value) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max()) {
        return false;
    }

    value = static_cast<int32_t>(parsed);
    return true;
}

bool ParseBool(const char* text, bool& value) {
    if (text == nullptr) {
        return false;
    }

    if (std::strcmp(text, "1") == 0 || std::strcmp(text, "true") == 0 ||
        std::strcmp(text, "on") == 0) {
        value = true;
        return true;
    }
    if (std::strcmp(text, "0") == 0 || std::strcmp(text, "false") == 0 ||
        std::strcmp(text, "off") == 0) {
        value = false;
        return true;
    }
    return false;
}

bool IsType(const char* type) {
    return type != nullptr &&
           (std::strcmp(type, "string") == 0 || std::strcmp(type, "str") == 0 ||
            std::strcmp(type, "int") == 0 || std::strcmp(type, "int32") == 0 ||
            std::strcmp(type, "bool") == 0);
}

bool ValidateSettings(const Settings& settings, const char* name) {
    if (settings.IsValid()) {
        return true;
    }
    std::printf("Namespace is unavailable: %s\n", name);
    return false;
}

const char* NvsTypeName(nvs_type_t type) {
    switch (type) {
        case NVS_TYPE_U8: return "u8";
        case NVS_TYPE_I8: return "i8";
        case NVS_TYPE_U16: return "u16";
        case NVS_TYPE_I16: return "i16";
        case NVS_TYPE_U32: return "u32";
        case NVS_TYPE_I32: return "int";
        case NVS_TYPE_U64: return "u64";
        case NVS_TYPE_I64: return "i64";
        case NVS_TYPE_STR: return "string";
        case NVS_TYPE_BLOB: return "blob";
        default: return "unknown";
    }
}

void PrintEntry(const nvs_entry_info_t& info) {
    std::printf("%s/%s (%s): ", info.namespace_name, info.key, NvsTypeName(info.type));

    Settings settings(info.namespace_name, false);
    if (!settings.IsValid()) {
        std::printf("<unavailable>\n");
        return;
    }

    switch (info.type) {
        case NVS_TYPE_STR:
            std::printf("%s\n", settings.GetString(info.key).c_str());
            break;
        case NVS_TYPE_I32:
            std::printf("%ld\n", static_cast<long>(settings.GetInt(info.key)));
            break;
        case NVS_TYPE_U8:
            std::printf("%s\n", settings.GetBool(info.key) ? "true" : "false");
            break;
        default:
            std::printf("<unsupported display type>\n");
            break;
    }
}

int HandleShow(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && !IsValidNvsName(argv[1]))) {
        PrintUsage();
        return 1;
    }

    const char* namespace_name = argc == 2 ? argv[1] : nullptr;
    nvs_iterator_t iterator = nullptr;
    esp_err_t ret = nvs_entry_find("nvs", namespace_name, NVS_TYPE_ANY, &iterator);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        std::printf("no kv item%s\n", namespace_name == nullptr ? "" : " in namespace");
        return 0;
    }
    if (ret != ESP_OK) {
        std::printf("failed to enumerate NVS: %s\n", esp_err_to_name(ret));
        return 1;
    }

    while (ret == ESP_OK && iterator != nullptr) {
        nvs_entry_info_t info{};
        if (nvs_entry_info(iterator, &info) == ESP_OK) {
            PrintEntry(info);
        }
        ret = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    return ret == ESP_ERR_NVS_NOT_FOUND ? 0 : 1;
}

int HandleClean(int argc, char** argv) {
    if (argc != 2 || !IsValidNvsName(argv[1])) {
        PrintUsage();
        return 1;
    }
    Settings settings(argv[1], true);
    if (!ValidateSettings(settings, argv[1])) {
        return 1;
    }
    settings.EraseAll();
    std::printf("flash clear %s success\n", argv[1]);
    return 0;
}

int HandleDelete(int argc, char** argv) {
    if (argc != 3 || !IsValidNvsName(argv[1]) || !IsValidNvsName(argv[2])) {
        PrintUsage();
        return 1;
    }
    Settings settings(argv[1], true);
    if (!ValidateSettings(settings, argv[1])) {
        return 1;
    }
    settings.EraseKey(argv[2]);
    std::printf("flash delete %s/%s success\n", argv[1], argv[2]);
    return 0;
}

int HandleSet(int argc, char** argv) {
    if (argc != 5 || !IsType(argv[1]) || !IsValidNvsName(argv[2]) ||
        !IsValidNvsName(argv[3])) {
        PrintUsage();
        return 1;
    }

    Settings settings(argv[2], true);
    if (!ValidateSettings(settings, argv[2])) {
        return 1;
    }

    const char* type = argv[1];
    const char* name = argv[3];
    const char* value = argv[4];
    if (std::strcmp(type, "string") == 0 || std::strcmp(type, "str") == 0) {
        settings.SetString(name, value);
    } else if (std::strcmp(type, "int") == 0 || std::strcmp(type, "int32") == 0) {
        int32_t parsed = 0;
        if (!ParseInt32(value, parsed)) {
            std::printf("invalid int value: %s\n", value);
            return 1;
        }
        settings.SetInt(name, parsed);
    } else {
        bool parsed = false;
        if (!ParseBool(value, parsed)) {
            std::printf("invalid bool value: %s (use true/false or 1/0)\n", value);
            return 1;
        }
        settings.SetBool(name, parsed);
    }

    std::printf("flash set %s/%s success\n", argv[2], name);
    return 0;
}

int HandleGet(int argc, char** argv) {
    if (argc != 4 || !IsType(argv[1]) || !IsValidNvsName(argv[2]) ||
        !IsValidNvsName(argv[3])) {
        PrintUsage();
        return 1;
    }

    Settings settings(argv[2], false);
    if (!ValidateSettings(settings, argv[2])) {
        return 1;
    }

    const char* type = argv[1];
    const char* name = argv[3];
    if (std::strcmp(type, "string") == 0 || std::strcmp(type, "str") == 0) {
        std::printf("flash get %s/%s: %s\n", argv[2], name, settings.GetString(name).c_str());
    } else if (std::strcmp(type, "int") == 0 || std::strcmp(type, "int32") == 0) {
        std::printf("flash get %s/%s: %ld\n", argv[2], name,
                    static_cast<long>(settings.GetInt(name)));
    } else {
        std::printf("flash get %s/%s: %s\n", argv[2], name,
                    settings.GetBool(name) ? "true" : "false");
    }
    return 0;
}

int HandleKvCommand(int argc, char** argv) {
    if (argc <= 1 || std::strcmp(argv[1], "help") == 0) {
        PrintUsage();
        return 0;
    }

    if (std::strcmp(argv[1], "show") == 0) {
        return HandleShow(argc - 1, argv + 1);
    }
    if (std::strcmp(argv[1], "clean") == 0) {
        return HandleClean(argc - 1, argv + 1);
    }
    if (std::strcmp(argv[1], "del") == 0) {
        return HandleDelete(argc - 1, argv + 1);
    }
    if (std::strcmp(argv[1], "set") == 0) {
        return HandleSet(argc - 1, argv + 1);
    }
    if (std::strcmp(argv[1], "get") == 0) {
        return HandleGet(argc - 1, argv + 1);
    }

    PrintUsage();
    return 1;
}

esp_err_t RegisterKvCommand(const char* name, const char* help) {
    const esp_console_cmd_t command = {
        .command = name,
        .help = help,
        .hint = "<show|clean|del|set|get|help> ...",
        .func = HandleKvCommand,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    return esp_console_cmd_register(&command);
}

}  // namespace

esp_err_t RegisterKvCommands() {
    return RegisterKvCommand("kv", "Read and write NVS key-value settings");
}
