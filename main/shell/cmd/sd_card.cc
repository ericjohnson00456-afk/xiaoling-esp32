#include "sd_card.h"

#include "application.h"
#include "sd_card/sd_card_manager.h"
#include "sd_card/sd_music_service.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <esp_console.h>
#include <esp_log.h>

#define TAG "SdCommand"

namespace {

void PrintUsage() {
    std::printf(
        "SD card commands:\n"
        "  sd scan          Mount the card and print every file recursively\n"
        "  sd play PATH     Play an audio file, for example: sd play music/test.mp3\n"
        "  sd stop          Stop SD card audio playback\n"
        "  sd clear-fingerprint  Clear saved TF card sync fingerprint\n"
        "  sd help          Show this help\n"
        "Paths containing spaces must be enclosed in quotes.\n");
}

bool ContainsParentDirectory(const std::string& path) {
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find('/', start);
        const std::string segment = path.substr(start, end - start);
        if (segment == "..") {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

bool ResolvePath(const char* argument, std::string& path) {
    if (argument == nullptr || argument[0] == '\0') {
        return false;
    }
    std::string value(argument);
    constexpr char kFilePrefix[] = "file://";
    if (value.rfind(kFilePrefix, 0) == 0) {
        value.erase(0, sizeof(kFilePrefix) - 1);
        if (value.empty() || value.front() != '/') {
            value.insert(value.begin(), '/');
        }
    } else if (value.rfind("/sdcard/", 0) != 0 && value != "/sdcard") {
        if (value.rfind("sdcard/", 0) == 0) {
            value.insert(value.begin(), '/');
        } else {
            value = "/sdcard/" + value;
        }
    }
    if (ContainsParentDirectory(value) ||
        (value.rfind("/sdcard/", 0) != 0 && value != "/sdcard")) {
        return false;
    }
    path = std::move(value);
    return true;
}

bool EnsureMounted(SdCardManager& manager) {
    if (manager.IsMounted()) {
        return true;
    }
    const esp_err_t ret = manager.Initialize();
    if (ret != ESP_OK) {
        std::printf("SD card mount failed: %s\n", esp_err_to_name(ret));
        return false;
    }
    return true;
}

int HandleSdCardCommand(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "help") == 0) {
        PrintUsage();
        return 0;
    }

    auto& manager = SdCardManager::GetInstance();
    if (std::strcmp(argv[1], "scan") == 0 && argc == 2) {
        if (!EnsureMounted(manager)) {
            return 1;
        }
        std::vector<SdCardFile> files;
        const esp_err_t ret = manager.ScanFiles(files);
        ESP_LOGI(TAG, "SD 卡扫描完成: result=%s, files=%u",
                 esp_err_to_name(ret), static_cast<unsigned>(files.size()));
        for (size_t index = 0; index < files.size(); ++index) {
            ESP_LOGI(TAG, "[%u] %s (%lu bytes)",
                     static_cast<unsigned>(index), files[index].path.c_str(),
                     static_cast<unsigned long>(files[index].size));
        }
        return ret == ESP_OK ? 0 : 1;
    }

    if (std::strcmp(argv[1], "play") == 0 && argc == 3) {
        if (!EnsureMounted(manager)) {
            return 1;
        }
        std::string path;
        if (!ResolvePath(argv[2], path)) {
            std::printf("Invalid SD card path: %s\n", argv[2]);
            return 1;
        }
        struct stat info = {};
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            std::printf("SD card file does not exist: %s\n", path.c_str());
            return 1;
        }
        const uint32_t session_id =
            Application::GetInstance().GetAudioPlayer().PlayMusicFile(path);
        if (session_id == 0) {
            std::printf("Failed to start SD card audio: %s\n", path.c_str());
            return 1;
        }
        ESP_LOGI(TAG, "SD 卡音频已提交: session=%lu, path=%s",
                 static_cast<unsigned long>(session_id), path.c_str());
        return 0;
    }

    if (std::strcmp(argv[1], "stop") == 0 && argc == 2) {
        const bool stopped = Application::GetInstance()
                                 .GetAudioPlayer()
                                 .StopLocalFilePlayback();
        std::printf("SD card audio stop: %s\n", stopped ? "ok" : "failed");
        return stopped ? 0 : 1;
    }

    if (std::strcmp(argv[1], "clear-fingerprint") == 0 && argc == 2) {
        const bool cleared = SdMusicService::GetInstance().ClearFingerprint();
        std::printf("SD card sync fingerprint clear: %s\n",
                    cleared ? "ok" : "failed");
        return cleared ? 0 : 1;
    }

    PrintUsage();
    return 1;
}

}  // namespace

esp_err_t RegisterSdCardCommand() {
    if (!SdCardManager::GetInstance().IsConfigured()) {
        return ESP_OK;
    }

    const esp_console_cmd_t command = {
        .command = "sd",
        .help = "Scan and play audio files from the SD card",
        .hint = "<scan|play|stop|clear-fingerprint|help> [path]",
        .func = HandleSdCardCommand,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    const esp_err_t ret = esp_console_cmd_register(&command);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}
