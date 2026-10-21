#include "wake_record.h"

#include "application.h"
#include "audio/wake_audio_recorder.h"
#include "audio/wake_recording_wav.h"
#include "sd_card/sd_card_manager.h"

#include <cstdio>
#include <cstring>
#include <esp_console.h>

#if CONFIG_USE_MICRO_WAKE_WORD
namespace {
void PrintStatus() {
    const auto status = WakeAudioRecorder::GetInstance().GetStatus();
    std::printf("Wake recording: %s, path=%s, PCM bytes=%lu (%.2f s), queued=%u samples\n",
                status.recording ? "recording" : (status.finishing ? "finishing" : "stopped"),
                status.path.empty() ? "-" : status.path.c_str(),
                static_cast<unsigned long>(status.data_bytes),
                status.data_bytes / (2.0 * wake_recording::kSampleRate),
                static_cast<unsigned>(status.pending_samples));
    if (!status.error.empty()) std::printf("Recording error: %s\n", status.error.c_str());
}

int HandleWakeRecord(int argc, char** argv) {
    auto& recorder = WakeAudioRecorder::GetInstance();
    if (argc == 2 && std::strcmp(argv[1], "start") == 0) {
        const bool started = recorder.Start();
        if (!started) std::printf("Cannot start recording (already active/finishing, or error below).\n");
        PrintStatus();
        if (started && !Application::GetInstance().GetAudioService().IsWakeWordRunning()) {
            std::printf("Wake model is paused; waiting for model PCM.\n");
        }
        return started ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "stop") == 0) {
        const bool stopped = recorder.Stop();
        PrintStatus();
        return stopped ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
        PrintStatus();
        return 0;
    }
    std::printf("wake-record start   Record wake model PCM to /sdcard/recordings/wake-NNNNN.wav\n"
                "wake-record stop    Drain PCM and finalize the WAV file before removing the card\n"
                "wake-record status  Show recording path, duration and errors\n");
    return argc == 2 && std::strcmp(argv[1], "help") == 0 ? 0 : 1;
}
}
#endif

esp_err_t RegisterWakeRecordCommand() {
#if CONFIG_USE_MICRO_WAKE_WORD
    if (!SdCardManager::GetInstance().IsConfigured()) return ESP_OK;
    const esp_console_cmd_t command = {
        .command = "wake-record",
        .help = "Record the PCM consumed by microWakeWord to a TF card WAV file",
        .hint = "<start|stop|status|help>",
        .func = HandleWakeRecord,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    return esp_console_cmd_register(&command);
#else
    return ESP_OK;
#endif
}
