#include "tasks.h"

#include "system/system_info.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_console.h>
#include <freertos/FreeRTOS.h>

namespace {

constexpr unsigned long kDefaultCpuSampleMs = 1000;
constexpr unsigned long kMinimumCpuSampleMs = 100;
constexpr unsigned long kMaximumCpuSampleMs = 10000;

void PrintUsage() {
    std::printf(
        "Usage:\n"
        "  tasks\n"
        "  tasks cpu [sample_ms]\n"
        "\n"
        "tasks              Print task count and a task snapshot.\n"
        "tasks cpu          Sample per-task CPU usage for 1000 ms.\n");
}

bool ParseSampleDuration(const char* text, unsigned long& duration_ms) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < kMinimumCpuSampleMs || value > kMaximumCpuSampleMs) {
        return false;
    }
    duration_ms = value;
    return true;
}

int HandleTasksCommand(int argc, char** argv) {
    if (argc == 1) {
        return SystemInfo::PrintTaskList() == ESP_OK ? 0 : 1;
    }
    if (std::strcmp(argv[1], "help") == 0) {
        PrintUsage();
        return 0;
    }
    if (std::strcmp(argv[1], "cpu") == 0) {
        if (argc > 3) {
            PrintUsage();
            return 1;
        }
        unsigned long duration_ms = kDefaultCpuSampleMs;
        if (argc == 3 && !ParseSampleDuration(argv[2], duration_ms)) {
            std::printf("sample_ms must be between %lu and %lu\n",
                        kMinimumCpuSampleMs, kMaximumCpuSampleMs);
            return 1;
        }
        std::printf("Sampling task CPU usage for %lu ms...\n", duration_ms);
        return SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(duration_ms)) == ESP_OK ? 0 : 1;
    }

    PrintUsage();
    return 1;
}

}  // namespace

esp_err_t RegisterTaskCommands() {
    const esp_console_cmd_t command = {
        .command = "tasks",
        .help = "Show FreeRTOS tasks and CPU usage",
        .hint = "[cpu [sample_ms]|help]",
        .func = HandleTasksCommand,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    return esp_console_cmd_register(&command);
}
