#include "device.h"

#include "settings.h"
#include "system/system_info.h"

#include <cstdio>
#include <cstring>

#include <esp_console.h>

namespace {

constexpr char kUserNamespace[] = "user";

bool IsValidId(const char* value) {
    return value != nullptr && std::strlen(value) == 36;
}

void PrintUsage() {
    std::printf(
        "Usage:\n"
        "  device set_pid <product_id>\n"
        "  device set_sid <secret_id>\n"
        "  device get_pid\n"
        "  device get_sid\n");
}

int HandleDeviceCommand(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "help") == 0) {
        PrintUsage();
        return 0;
    }

    const bool is_pid = std::strcmp(argv[1], "set_pid") == 0;
    const bool is_sid = std::strcmp(argv[1], "set_sid") == 0;
    if (is_pid || is_sid) {
        if (argc != 3 || !IsValidId(argv[2])) {
            std::printf("Invalid %s: expected a 36-character UUID\n",
                        is_pid ? "product_id" : "secret_id");
            return 1;
        }
        Settings settings(kUserNamespace, true);
        if (!settings.IsValid()) {
            std::printf("Failed to open user settings\n");
            return 1;
        }
        settings.SetString(is_pid ? "pid" : "sid", argv[2]);
        std::printf("Set %s success. Reboot to apply it to the active connection.\n",
                    is_pid ? "product_id" : "secret_id");
        return 0;
    }

    if (std::strcmp(argv[1], "get_pid") == 0 || std::strcmp(argv[1], "get_sid") == 0) {
        if (argc != 2) {
            PrintUsage();
            return 1;
        }
        const bool pid = std::strcmp(argv[1], "get_pid") == 0;
        const std::string value = pid ? SystemInfo::GetProductId() : SystemInfo::GetSecretId();
        std::printf("%s=%s\n", pid ? "product_id" : "secret_id", value.c_str());
        return 0;
    }

    PrintUsage();
    return 1;
}

}  // namespace

esp_err_t RegisterDeviceCommands() {
    const esp_console_cmd_t command = {
        .command = "device",
        .help = "Set runtime product and secret IDs",
        .hint = "<set_pid|set_sid|get_pid|get_sid|help> ...",
        .func = HandleDeviceCommand,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    return esp_console_cmd_register(&command);
}
