#include "reboot.h"

#include <cstdio>

#include <esp_console.h>
#include <esp_system.h>

namespace {

int HandleRebootCommand(int, char**) {
    std::printf("rebooting...\n");
    std::fflush(stdout);
    esp_restart();
    return 0;
}

}  // namespace

esp_err_t RegisterRebootCommand() {
    const esp_console_cmd_t command = {
        .command = "reboot",
        .help = "Restart the device",
        .hint = nullptr,
        .func = HandleRebootCommand,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    return esp_console_cmd_register(&command);
}
