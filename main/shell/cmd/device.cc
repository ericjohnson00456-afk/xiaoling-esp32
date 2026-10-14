#include "device.h"

#include "application.h"
#include "xiaoling/session/voice_session_epoch.h"
#include "event/app_event.h"
#include "system/settings.h"
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
        "  device get_sid\n"
        "  device get_device_id\n"
        "  device audio_test <start|stop>\n"
        "  device audio_stress <start|stop>\n"
        "  device wake <wake_word>\n"
        "  device tts_test <http_url>\n");
}

int HandleDeviceCommand(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "help") == 0) {
        PrintUsage();
        return 0;
    }
    if (std::strcmp(argv[1], "audio_stress") == 0) {
        if (argc != 3 || (std::strcmp(argv[2], "start") != 0 && std::strcmp(argv[2], "stop") != 0)) {
            PrintUsage();
            return 1;
        }
        Application::GetInstance().SetAudioStressMode(std::strcmp(argv[2], "start") == 0);
        return 0;
    }
    if (std::strcmp(argv[1], "wake") == 0) {
        if (argc != 3 || argv[2][0] == '\0') {
            PrintUsage();
            return 1;
        }
        Application::GetInstance().WakeWordInvoke(argv[2]);
        return 0;
    }
    if (std::strcmp(argv[1], "tts_test") == 0) {
        if (argc != 3 || (std::strncmp(argv[2], "http://", 7) != 0 &&
                          std::strncmp(argv[2], "https://", 8) != 0)) {
            PrintUsage();
            return 1;
        }
        auto& app = Application::GetInstance();
        const auto generation = Xiaoling::voice_session_epoch.Current();
        app.Schedule([&app, url = std::string(argv[2]), generation]() {
            if (!Xiaoling::voice_session_epoch.IsCurrent(generation)) return;
            if (app.GetDeviceState() != kDeviceStateIdle &&
                app.GetDeviceState() != kDeviceStateListening) {
                std::printf("TTS test requires idle/listening state\n");
                return;
            }
            AppEvent::PublishTextScoped(AppEvent::Id::XiaolingTtsUrl, url,
                                        Xiaoling::voice_session_epoch, generation);
        });
        return 0;
    }

    if (std::strcmp(argv[1], "audio_test") == 0) {
        if (argc != 3 || (std::strcmp(argv[2], "start") != 0 &&
                          std::strcmp(argv[2], "stop") != 0)) {
            PrintUsage();
            return 1;
        }
        const bool start = std::strcmp(argv[2], "start") == 0;
        auto& app = Application::GetInstance();
        app.Schedule([start, &app]() {
            const auto state = app.GetDeviceState();
            if (start && state == kDeviceStateWifiConfiguring) {
                app.StartListening();
            } else if (!start && state == kDeviceStateAudioTesting) {
                app.StopListening();
            } else {
                std::printf("Audio test %s ignored in state %s\n",
                            start ? "start" : "stop",
                            DeviceStatus::GetStateName(state));
            }
        });
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

    if (std::strcmp(argv[1], "get_device_id") == 0) {
        if (argc != 2) {
            PrintUsage();
            return 1;
        }
        std::printf("device_id=%s\n", SystemInfo::GetCloudDeviceId().c_str());
        return 0;
    }

    PrintUsage();
    return 1;
}

}  // namespace

esp_err_t RegisterDeviceCommands() {
    const esp_console_cmd_t command = {
        .command = "device",
        .help = "Set or inspect runtime device identity",
        .hint = "<set_pid|set_sid|get_pid|get_sid|get_device_id|help> ...",
        .func = HandleDeviceCommand,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    return esp_console_cmd_register(&command);
}
