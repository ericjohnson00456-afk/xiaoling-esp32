#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "sd_music_types.h"

class SdMusicService {
public:
    using AuthorizationProvider = std::function<std::string()>;

    static SdMusicService& GetInstance();

    void Initialize(AuthorizationProvider authorization_provider);
    void OnCardInserted();
    void OnCardRemoved();
    void OnUnsupportedFileSystem();
    void OnCloudReady();
    void OnCloudDisconnected();

    // 清除本地保存的 TF 卡同步指纹。卡仍在位时会立即重新同步。
    bool ClearFingerprint();

    bool IsBusy() const { return busy_.load(); }
    bool IsCardReady() const { return card_ready_.load(); }
    bool WaitForFileAccessToStop(uint32_t timeout_ms) const;

private:
    struct WorkerContext;
    struct RemovedContext;

    SdMusicService() = default;

    void TryStartSync();
    void RunSync(uint32_t generation, const std::string& authorization,
                 bool boot_initial_card);
    void FinishWorker(uint32_t generation);
    void PublishState(const SdMusicSyncState& state) const;
    void ReportRemovedAsync(std::string card_id);
    void ReconcileBootRemoval(const std::string& authorization);
    bool IsCurrent(uint32_t generation) const;
    static void SyncTaskEntry(void* arg);
    static void RemovedTaskEntry(void* arg);

    mutable std::mutex mutex_;
    AuthorizationProvider authorization_provider_;
    std::string current_card_id_;
    TaskHandle_t worker_task_ = nullptr;
    bool initialized_ = false;
    bool pending_sync_ = false;
    bool boot_initial_card_pending_ = false;
    bool boot_state_reconciled_ = false;
    std::atomic_uint32_t generation_{0};
    std::atomic_bool cancelled_{false};
    std::atomic_bool file_access_active_{false};
    std::atomic_bool busy_{false};
    std::atomic_bool card_ready_{false};
    std::atomic_bool cloud_ready_{false};
};
