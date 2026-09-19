#include "kv_user.h"

#include "application.h"
#include "settings.h"
#include "ui/ui_app.h"
#include "xiaoling/session/session_core.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <esp_console.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace {

constexpr size_t kNvsNameMaxLength = 15;
constexpr char kRemovedNetcfgModeKey[] = "netcfg_mode";

bool IsRemovedNetcfgModeKey(const char* namespace_name, const char* key) {
    return namespace_name != nullptr && key != nullptr &&
           std::strcmp(namespace_name, KvUser::kNamespace) == 0 &&
           std::strcmp(key, kRemovedNetcfgModeKey) == 0;
}

void PrintUsage() {
    std::printf(
        "Usage:\n"
        "  kv show [namespace]\n"
        "  kv clean\n"
        "  kv del <namespace> <key>\n"
        "  kv set <string|int|bool> <namespace>.<key> <value>\n"
        "  kv get <string|int|bool> <namespace>.<key>\n"
        "  kv set <string|int|bool> <namespace> <key> <value>\n"
        "  kv get <string|int|bool> <namespace> <key>\n"
        "  kv ota <show|get|set|help> ...\n"
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

enum class UserIntKey {
    None,
    IntMode,
    UiLanguage,
};

UserIntKey ParseUserIntKey(const char* logical_key) {
    if (logical_key == nullptr) return UserIntKey::None;
    if (std::strcmp(logical_key, KvUser::kIntMode) == 0) return UserIntKey::IntMode;
    if (std::strcmp(logical_key, KvUser::kUiLanguage) == 0) return UserIntKey::UiLanguage;
    return UserIntKey::None;
}

bool IsValidUserIntValue(UserIntKey key, int32_t value) {
    switch (key) {
        case UserIntKey::IntMode:
            return value >= 0 && value <= 2;
        case UserIntKey::UiLanguage:
            return value >= 0 && value <= 1;
        case UserIntKey::None:
            return false;
    }
    return false;
}

void SetUserIntValue(UserIntKey key, int32_t value) {
    switch (key) {
        case UserIntKey::IntMode:
            KvUser::SetIntMode(value);
            Application::GetInstance().SetDuplexMode(
                value == 0 ? DuplexMode::kFullDuplex :
                value == 1 ? DuplexMode::kHalfDuplex : DuplexMode::kSimplex);
            break;
        case UserIntKey::UiLanguage:
            KvUser::SetUiLanguage(value);
            UiApp::SetLanguageMode(value);
            break;
        case UserIntKey::None:
            break;
    }
}

int32_t GetUserIntValue(UserIntKey key) {
    switch (key) {
        case UserIntKey::IntMode: return KvUser::GetIntMode();
        case UserIntKey::UiLanguage: return KvUser::GetUiLanguage();
        case UserIntKey::None: return -1;
    }
    return -1;
}

const char* UserIntKeyName(UserIntKey key) {
    switch (key) {
        case UserIntKey::IntMode: return KvUser::kIntMode;
        case UserIntKey::UiLanguage: return KvUser::kUiLanguage;
        case UserIntKey::None: return "";
    }
    return "";
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

int HandleClean(int argc, char**) {
    if (argc != 1) {
        PrintUsage();
        return 1;
    }

    // Wipe the whole default NVS partition, then bring it back fresh. Handles
    // opened by other modules become stale, so a reboot is required.
    esp_err_t ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        std::printf("failed to erase NVS: %s\n", esp_err_to_name(ret));
        return 1;
    }
    ret = nvs_flash_init();
    if (ret != ESP_OK) {
        std::printf("failed to re-initialize NVS: %s\n", esp_err_to_name(ret));
        return 1;
    }
    std::printf("NVS erased. Run 'reboot' to start clean.\n");
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
    if ((argc != 4 && argc != 5) || !IsType(argv[1])) {
        PrintUsage();
        return 1;
    }

    // Accept the compact form: kv set int user.disable_tone_update 1.
    if (argc == 4 && (std::strcmp(argv[1], "int") == 0 ||
                      std::strcmp(argv[1], "int32") == 0)) {
        const char* separator = std::strchr(argv[2], '.');
        if (separator != nullptr && separator != argv[2] && separator[1] != '\0') {
            const std::string namespace_name(argv[2], separator - argv[2]);
            const char* key = separator + 1;
            if (IsRemovedNetcfgModeKey(namespace_name.c_str(), key)) {
                std::printf("unsupported key: %s.%s\n", namespace_name.c_str(), key);
                return 1;
            }
            if (namespace_name == KvUser::kNamespace && std::strcmp(key, "device_mode") == 0) {
                int32_t value = 0;
                if (!ParseInt32(argv[3], value)) {
                    std::printf("invalid int value: %s\n", argv[3]);
                    return 1;
                }
                KvUser::SetDeviceMode(value);
                std::printf("flash set %s=%ld success\n", KvUser::kDeviceMode,
                             static_cast<long>(value));
                return 0;
            }
            if (namespace_name == KvUser::kNamespace &&
                (std::strcmp(argv[2], KvUser::kDisableAppUpdate) == 0 ||
                 std::strcmp(argv[2], KvUser::kDisableWakewordUpdate) == 0 ||
                 std::strcmp(argv[2], KvUser::kDisableToneUpdate) == 0 ||
                 std::strcmp(argv[2], KvUser::kDisableEmojiUpdate) == 0)) {
                KvOta::UpdateType type;
                if (std::strcmp(key, "disable_app_update") == 0) type = KvOta::UpdateType::App;
                else if (std::strcmp(key, "disable_wakeword_update") == 0) type = KvOta::UpdateType::WakeWord;
                else if (std::strcmp(key, "disable_tone_update") == 0) type = KvOta::UpdateType::Tone;
                else type = KvOta::UpdateType::Emoji;
                int32_t value = 0;
                if (!ParseInt32(argv[3], value)) {
                    std::printf("invalid int value: %s\n", argv[3]);
                    return 1;
                }
                KvOta::SetDisabled(type, value);
                std::printf("flash set %s=%ld success\n", KvOta::LogicalKey(type),
                             static_cast<long>(value));
                return 0;
            }
            if (namespace_name == KvUser::kNamespace) {
                const UserIntKey user_key = ParseUserIntKey(argv[2]);
                if (user_key != UserIntKey::None) {
                    int32_t value = 0;
                    if (!ParseInt32(argv[3], value) || !IsValidUserIntValue(user_key, value)) {
                        std::printf("invalid value for %s\n", UserIntKeyName(user_key));
                        return 1;
                    }
                    SetUserIntValue(user_key, value);
                    std::printf("flash set %s=%ld success\n", UserIntKeyName(user_key),
                                 static_cast<long>(value));
                    return 0;
                }
            }
        }
        PrintUsage();
        return 1;
    }

    if (argc != 5 || !IsValidNvsName(argv[2])) {
        PrintUsage();
        return 1;
    }

    if (IsRemovedNetcfgModeKey(argv[2], argv[3])) {
        std::printf("unsupported key: %s.%s\n", argv[2], argv[3]);
        return 1;
    }

    if (std::strcmp(argv[1], "int") == 0 &&
        std::strcmp(argv[2], KvUser::kNamespace) == 0) {
        if (std::strcmp(argv[3], "device_mode") == 0) {
            int32_t value = 0;
            if (!ParseInt32(argv[4], value)) {
                std::printf("invalid int value: %s\n", argv[4]);
                return 1;
            }
            KvUser::SetDeviceMode(value);
            std::printf("flash set %s/%s success\n", argv[2], argv[3]);
            return 0;
        }
        const std::string logical_key = std::string(KvUser::kNamespace) + "." + argv[3];
        const UserIntKey user_key = ParseUserIntKey(logical_key.c_str());
        if (user_key != UserIntKey::None) {
            int32_t value = 0;
            if (!ParseInt32(argv[4], value) || !IsValidUserIntValue(user_key, value)) {
                std::printf("invalid value for %s\n", UserIntKeyName(user_key));
                return 1;
            }
            SetUserIntValue(user_key, value);
            std::printf("flash set %s=%ld success\n", UserIntKeyName(user_key),
                         static_cast<long>(value));
            return 0;
        }
        KvOta::UpdateType type;
        if (logical_key == KvOta::LogicalKey(KvOta::UpdateType::App) ||
            logical_key == KvOta::LogicalKey(KvOta::UpdateType::WakeWord) ||
            logical_key == KvOta::LogicalKey(KvOta::UpdateType::Tone) ||
            logical_key == KvOta::LogicalKey(KvOta::UpdateType::Emoji)) {
            int32_t value = 0;
            char* end = nullptr;
            errno = 0;
            const long parsed = std::strtol(argv[4], &end, 10);
            if (errno != 0 || end == argv[4] || *end != '\0' ||
                parsed < std::numeric_limits<int32_t>::min() ||
                parsed > std::numeric_limits<int32_t>::max()) {
                std::printf("invalid int value: %s\n", argv[4]);
                return 1;
            }
            value = static_cast<int32_t>(parsed);
            if (logical_key == KvOta::LogicalKey(KvOta::UpdateType::App)) type = KvOta::UpdateType::App;
            else if (logical_key == KvOta::LogicalKey(KvOta::UpdateType::WakeWord)) type = KvOta::UpdateType::WakeWord;
            else if (logical_key == KvOta::LogicalKey(KvOta::UpdateType::Tone)) type = KvOta::UpdateType::Tone;
            else type = KvOta::UpdateType::Emoji;
            KvOta::SetDisabled(type, value);
            std::printf("flash set %s/%s success\n", argv[2], argv[3]);
            return 0;
        }
    }

    if (!IsValidNvsName(argv[3])) {
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
    if ((argc != 3 && argc != 4) || !IsType(argv[1])) {
        PrintUsage();
        return 1;
    }

    // Accept the compact form: kv get int user.disable_tone_update.
    if (argc == 3 && (std::strcmp(argv[1], "int") == 0 ||
                      std::strcmp(argv[1], "int32") == 0)) {
        const char* separator = std::strchr(argv[2], '.');
        if (separator != nullptr && separator != argv[2] && separator[1] != '\0' &&
            std::string(argv[2], separator - argv[2]) == "user") {
            const std::string logical_key = argv[2];
            if (IsRemovedNetcfgModeKey("user", logical_key.c_str() + 5)) {
                std::printf("unsupported key: %s\n", logical_key.c_str());
                return 1;
            }
            const KvOta::UpdateType types[] = {KvOta::UpdateType::App,
                                                KvOta::UpdateType::WakeWord,
                                                KvOta::UpdateType::Tone,
                                                KvOta::UpdateType::Emoji};
            for (const auto type : types) {
                if (logical_key == KvOta::LogicalKey(type)) {
                    std::printf("flash get %s: %ld\n", logical_key.c_str(),
                                 static_cast<long>(KvOta::GetValue(type)));
                    return 0;
                }
            }
            if (logical_key == KvUser::kDeviceMode) {
                std::printf("flash get %s: %ld\n", logical_key.c_str(),
                            static_cast<long>(KvUser::GetDeviceMode()));
                return 0;
            }
            const UserIntKey user_key = ParseUserIntKey(logical_key.c_str());
            if (user_key != UserIntKey::None) {
                std::printf("flash get %s: %ld\n", logical_key.c_str(),
                             static_cast<long>(GetUserIntValue(user_key)));
                return 0;
            }
        }
        PrintUsage();
        return 1;
    }

    if (argc != 4 || !IsValidNvsName(argv[2])) {
        PrintUsage();
        return 1;
    }

    if (IsRemovedNetcfgModeKey(argv[2], argv[3])) {
        std::printf("unsupported key: %s.%s\n", argv[2], argv[3]);
        return 1;
    }

    if ((std::strcmp(argv[1], "int") == 0 || std::strcmp(argv[1], "int32") == 0) &&
        std::strcmp(argv[2], KvUser::kNamespace) == 0) {
        const std::string logical_key = std::string(KvUser::kNamespace) + "." + argv[3];
        if (logical_key == KvUser::kDeviceMode) {
            std::printf("flash get %s/%s: %ld\n", argv[2], argv[3],
                        static_cast<long>(KvUser::GetDeviceMode()));
            return 0;
        }
        const UserIntKey user_key = ParseUserIntKey(logical_key.c_str());
        if (user_key != UserIntKey::None) {
            std::printf("flash get %s/%s: %ld\n", argv[2], argv[3],
                        static_cast<long>(GetUserIntValue(user_key)));
            return 0;
        }
        const KvOta::UpdateType types[] = {KvOta::UpdateType::App,
                                            KvOta::UpdateType::WakeWord,
                                            KvOta::UpdateType::Tone,
                                            KvOta::UpdateType::Emoji};
        for (const auto type : types) {
            if (logical_key == KvOta::LogicalKey(type)) {
                // The OTA module owns the physical key mapping; expose its
                // value through the dedicated command for this legacy form.
                std::printf("flash get %s/%s: %d\n", argv[2], argv[3],
                            KvOta::IsDisabled(type) ? 1 : 0);
                return 0;
            }
        }
    }

    if (!IsValidNvsName(argv[3])) {
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
    if (std::strcmp(argv[1], "ota") == 0) {
        return KvOta::HandleKvOtaCommand(argc - 1, argv + 1);
    }

    PrintUsage();
    return 1;
}

esp_err_t RegisterKvCommand(const char* name, const char* help) {
    const esp_console_cmd_t command = {
        .command = name,
        .help = help,
        .hint = "<show|clean|del|set|get|ota|help> ...",
        .func = HandleKvCommand,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    return esp_console_cmd_register(&command);
}

}  // namespace

namespace KvUser {

int32_t GetDeviceMode() {
    Settings settings(KvUser::kNamespace, false);
    return settings.GetInt("device_mode", 0);
}

void SetDeviceMode(int32_t value) {
    Settings settings(KvUser::kNamespace, true);
    settings.SetInt("device_mode", value);
}

int32_t GetIntMode() {
    Settings settings(KvUser::kNamespace, false);
    return settings.GetInt("int_mode", -1);
}

void SetIntMode(int32_t value) {
    Settings settings(KvUser::kNamespace, true);
    settings.SetInt("int_mode", value);
}

int32_t GetUiLanguage() {
    Settings settings(KvUser::kNamespace, false);
    return settings.GetInt("ui_language", -1);
}

void SetUiLanguage(int32_t value) {
    Settings settings(KvUser::kNamespace, true);
    settings.SetInt("ui_language", value);
}

}  // namespace KvUser

esp_err_t RegisterKvCommands() {
    return RegisterKvCommand("kv", "Read and write NVS key-value settings");
}
