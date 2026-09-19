#include "wifi.h"

#include "application.h"
#include "ui/i18n/lang_config.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <esp_console.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

namespace {

bool EnsureWifiInitialized() {
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsInitialized()) {
        return true;
    }

    WifiManagerConfig config;
    config.ssid_prefix = "Xiaoling";
    config.language = Lang::CODE;
    if (!wifi.Initialize(config)) {
        std::printf("Failed to initialize Wi-Fi\n");
        return false;
    }
    return true;
}

void PrintUsage() {
    std::printf(
        "Usage:\n"
        "  wifi connect <ssid> <password>\n"
        "  wifi disconnect\n"
        "  wifi scan\n");
}

int HandleWifiCommand(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "help") == 0) {
        PrintUsage();
        return 0;
    }
    if (!EnsureWifiInitialized()) {
        return 1;
    }

    auto& wifi = WifiManager::GetInstance();
    if (std::strcmp(argv[1], "connect") == 0) {
        if (argc != 4 || argv[2][0] == '\0') {
            PrintUsage();
            return 1;
        }
        // Keep the same credential format used by BLUFI/web provisioning.
        SsidManager::GetInstance().AddSsid(argv[2], argv[3]);
        wifi.StopStation();
        wifi.StartStation();
        std::printf("Wi-Fi connection started for ssid=\"%s\"\n", argv[2]);
        return 0;
    }
    if (std::strcmp(argv[1], "disconnect") == 0) {
        if (argc != 2) {
            PrintUsage();
            return 1;
        }
        // Reuse the five-click reset path: clear credentials, reset the
        // Xiaoling connection, and enter the board's provisioning mode.
        Application::GetInstance().ClearNetworkConfiguration();
        std::printf("Wi-Fi disconnected; entering configuration mode\n");
        return 0;
    }
    if (std::strcmp(argv[1], "scan") != 0 || argc != 2) {
        PrintUsage();
        return 1;
    }

    // Stop the managed station first so its scan-done callback does not
    // consume the scan records or trigger an automatic reconnect.
    const bool restart_station = wifi.IsConnected() || !wifi.GetSsid().empty();
    wifi.StopConfigAp();
    wifi.StopStation();

    auto restore_station = [&]() {
        esp_wifi_stop();
        if (restart_station) {
            wifi.StartStation();
        }
    };

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        std::printf("Wi-Fi scan setup failed: %s\n", esp_err_to_name(ret));
        if (restart_station) {
            wifi.StartStation();
        }
        return 1;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE) {
        std::printf("Wi-Fi scan start failed: %s\n", esp_err_to_name(ret));
        if (restart_station) {
            wifi.StartStation();
        }
        return 1;
    }

    wifi_scan_config_t config{};
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 100;
    config.scan_time.active.max = 300;
    ret = esp_wifi_scan_start(&config, true);
    if (ret != ESP_OK) {
        std::printf("Wi-Fi scan failed: %s\n", esp_err_to_name(ret));
        restore_station();
        return 1;
    }

    uint16_t count = 0;
    ret = esp_wifi_scan_get_ap_num(&count);
    if (ret != ESP_OK) {
        std::printf("Wi-Fi scan results unavailable: %s\n", esp_err_to_name(ret));
        restore_station();
        return 1;
    }
    std::vector<wifi_ap_record_t> records(count);
    if (count > 0) {
        ret = esp_wifi_scan_get_ap_records(&count, records.data());
    }
    if (ret != ESP_OK) {
        std::printf("Wi-Fi scan results unavailable: %s\n", esp_err_to_name(ret));
        restore_station();
        return 1;
    }

    std::printf("Wi-Fi networks found: %u\n", static_cast<unsigned>(count));
    for (uint16_t index = 0; index < count; ++index) {
        const auto& record = records[index];
        const size_t ssid_length = strnlen(reinterpret_cast<const char*>(record.ssid),
                                           sizeof(record.ssid));
        std::printf("[%u] ssid=\"%.*s\" rssi=%d channel=%u auth=%d\n",
                    static_cast<unsigned>(index), static_cast<int>(ssid_length),
                    reinterpret_cast<const char*>(record.ssid), record.rssi,
                    static_cast<unsigned>(record.primary), static_cast<int>(record.authmode));
    }
    restore_station();
    return 0;
}

}  // namespace

esp_err_t RegisterWifiCommands() {
    const esp_console_cmd_t command = {
        .command = "wifi",
        .help = "Control Wi-Fi station and scan nearby networks",
        .hint = "<connect <ssid> <password>|disconnect|scan|help>",
        .func = HandleWifiCommand,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    return esp_console_cmd_register(&command);
}
