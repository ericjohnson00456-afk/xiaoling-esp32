#include "test.h"

#include "application.h"
#include "event/app_event.h"
#include "xiaoling/conn/cloud_tts_url.h"
#include "xiaoling/session/voice_session_epoch.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <esp_console.h>

namespace {

void PrintUsage() {
    std::printf(
        "Usage:\n"
        "  test audio <start|stop>\n"
        "  test audio_stress <start|stop>\n"
        "  test wake <wake_word>\n"
        "  test tts_url <http_url>\n"
        "  test tts_text <text>\n");
}

void QueueTtsTestUrl(Application& app, std::string url, uint32_t generation) {
    app.Schedule([&app, url = std::move(url), generation]() {
        if (!Xiaoling::voice_session_epoch.IsCurrent(generation)) return;
        if (app.GetDeviceState() != kDeviceStateIdle &&
            app.GetDeviceState() != kDeviceStateListening) {
            std::printf("TTS test requires idle/listening state\n");
            return;
        }
        const auto event_id = voice_player_url_is_mp3(url.c_str())
                                  ? AppEvent::Id::XiaolingPushTtsUrl
                                  : AppEvent::Id::XiaolingTtsUrl;
        AppEvent::PublishTextScoped(event_id, url,
                                    Xiaoling::voice_session_epoch, generation);
    });
}

int HandleTestCommand(int argc, char** argv) {
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
    if (std::strcmp(argv[1], "tts_url") == 0) {
        if (argc != 3 || (std::strncmp(argv[2], "http://", 7) != 0 &&
                          std::strncmp(argv[2], "https://", 8) != 0)) {
            PrintUsage();
            return 1;
        }
        auto& app = Application::GetInstance();
        const auto generation = Xiaoling::voice_session_epoch.Current();
        QueueTtsTestUrl(app, argv[2], generation);
        return 0;
    }
    if (std::strcmp(argv[1], "tts_text") == 0) {
        if (argc < 3) {
            PrintUsage();
            return 1;
        }
        std::string text;
        for (int i = 2; i < argc; ++i) {
            if (!text.empty()) text += ' ';
            text += argv[i];
        }
        auto& app = Application::GetInstance();
        const auto generation = Xiaoling::voice_session_epoch.Current();
        if (!app.RequestTextTtsUrl(text, [&app, generation](bool success, std::string url) {
                if (!success || url.empty()) {
                    std::printf("TTS text request failed\n");
                    return;
                }
                QueueTtsTestUrl(app, std::move(url), generation);
            })) {
            std::printf("TTS text request unavailable\n");
            return 1;
        }
        std::printf("TTS text request sent\n");
        return 0;
    }

    if (std::strcmp(argv[1], "audio") == 0) {
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

    PrintUsage();
    return 1;
}

}  // namespace

esp_err_t RegisterTestCommands() {
    const esp_console_cmd_t command = {
        .command = "test",
        .help = "Test audio capture, stress load, wake and TTS playback",
        .hint = "<audio|audio_stress|wake|tts_url|tts_text|help> ...",
        .func = HandleTestCommand,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    return esp_console_cmd_register(&command);
}
