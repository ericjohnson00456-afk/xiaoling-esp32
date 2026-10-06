#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sd_protocol_types.h>

#include "sd_card_config.h"

struct SdCardFile {
    std::string path;
    uint64_t size = 0;
};

struct SdCardInfo {
    std::string card_id;
    uint64_t capacity_bytes = 0;
    uint64_t filesystem_bytes = 0;
    uint64_t free_bytes = 0;
};

enum class SdCardEvent {
    kInserted,
    kRemoved,
    kUnsupportedFileSystem,
};

class SdCardManager {
public:
    using EventCallback = std::function<void(SdCardEvent)>;

    static SdCardManager& GetInstance();

    SdCardManager(const SdCardManager&) = delete;
    SdCardManager& operator=(const SdCardManager&) = delete;

    esp_err_t Initialize();
    esp_err_t Deinitialize();
    esp_err_t StartMonitoring(const SdSpiCardConfig& config,
                              EventCallback callback);
    esp_err_t ScanFiles(std::vector<SdCardFile>& files) const;
    esp_err_t GetCardInfo(SdCardInfo& info) const;
    bool IsConfigured() const;
    bool IsMounted() const;
    const char* MountPoint() const;

private:
    SdCardManager() = default;

    esp_err_t Mount(bool log_details);
    bool ProbeMountedCard() const;
    void Notify(SdCardEvent event);
    void MonitorLoop();
    static void MonitorTaskEntry(void* arg);
    static esp_err_t DoSharedSpiTransaction(int slot,
                                            sdmmc_command_t* command);

    mutable std::mutex mutex_;
    std::optional<SdSpiCardConfig> config_;
    void* card_ = nullptr;
    bool mounted_ = false;
    bool removal_pending_ = false;
    bool filesystem_error_notified_ = false;
    EventCallback event_callback_;
    std::atomic_bool monitoring_{false};
    TaskHandle_t monitor_task_ = nullptr;
};
