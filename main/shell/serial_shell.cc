#include "serial_shell.h"

#include "cmd/kv/kv_user.h"
#include "cmd/device.h"
#include "cmd/reboot.h"
#include "cmd/sd_card.h"
#include "cmd/tasks.h"
#include "cmd/test.h"
#include "cmd/wifi.h"
#include "cmd/wake_record.h"

#include <cstdio>
#include <fcntl.h>

#include <esp_console.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "SerialShell"

namespace {

void ShellTask(void*) {
    char line[256];
    size_t line_length = 0;
    bool discard_line = false;
    std::printf("\r\nXiaoling shell ready. Type 'help'.\r\nxiaoling> ");
    std::fflush(stdout);

    while (true) {
        const int ch = std::fgetc(stdin);
        if (ch == EOF) {
            // USB-Serial/JTAG can report EOF while no host is connected.
            // Keep the shell task alive and retry when the terminal appears.
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (ch != '\r' && ch != '\n') {
            if (discard_line) {
                continue;
            }
            if ((ch == '\b' || ch == 0x7f) && line_length > 0) {
                --line_length;
            } else if (line_length + 1 < sizeof(line)) {
                line[line_length++] = static_cast<char>(ch);
            } else {
                discard_line = true;
                line_length = 0;
                std::printf("\r\nCommand too long.\r\nxiaoling> ");
                std::fflush(stdout);
            }
            continue;
        }

        if (discard_line) {
            discard_line = false;
            continue;
        }
        // Ignore the LF half of CRLF and empty lines.
        if (line_length == 0) {
            continue;
        }
        line[line_length] = '\0';
        line_length = 0;

        int command_ret = 0;
        const esp_err_t ret = esp_console_run(line, &command_ret);
        if (ret == ESP_ERR_NOT_FOUND) {
            std::printf("Unknown command. Type 'help'.\r\n");
        } else if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) {
            std::printf("Command failed: %s\r\n", esp_err_to_name(ret));
        }
        std::printf("xiaoling> ");
        std::fflush(stdout);
    }

}

}  // namespace

SerialShell& SerialShell::GetInstance() {
    static SerialShell instance;
    return instance;
}

bool SerialShell::Initialize() {
    if (initialized_) {
        return true;
    }

    esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
    esp_err_t ret = esp_console_init(&console_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize console: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_console_register_help_command();
    if (ret == ESP_OK) {
        ret = RegisterKvCommands();
    }
    if (ret == ESP_OK) {
        ret = RegisterRebootCommand();
    }
    if (ret == ESP_OK) {
        ret = RegisterDeviceCommands();
    }
    if (ret == ESP_OK) {
        ret = RegisterTestCommands();
    }
    if (ret == ESP_OK) {
        ret = RegisterTaskCommands();
    }
    if (ret == ESP_OK) {
        ret = RegisterWifiCommands();
    }
    if (ret == ESP_OK) {
        ret = RegisterSdCardCommand();
    }
    if (ret == ESP_OK) {
        ret = RegisterWakeRecordCommand();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register shell commands: %s", esp_err_to_name(ret));
        esp_console_deinit();
        return false;
    }

    // The system console may leave stdin/stdout in non-blocking mode. The
    // command task must block while waiting for the next line of input.
    fcntl(fileno(stdin), F_SETFL, 0);
    fcntl(fileno(stdout), F_SETFL, 0);
    if (xTaskCreatePinnedToCore(ShellTask, "serial_shell", 4096, nullptr, 2, nullptr,
                                tskNO_AFFINITY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to start shell task");
        esp_console_deinit();
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Serial shell started");
    return true;
}
