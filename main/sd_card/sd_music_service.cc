#include "sd_music_service.h"

#include "sd_card_manager.h"
#include "sd_music_http.h"
#include "sd_music_scanner.h"
#include "system/psram_task.h"
#include "system/settings.h"
#include "ui/ui.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/semphr.h>

namespace {

constexpr char kTag[] = "SdMusicService";
constexpr char kSettingsNamespace[] = "sd_music";
constexpr char kCardIdKey[] = "cid";
constexpr char kStorageStampKey[] = "stamp";
constexpr char kPresentCardKey[] = "present";
constexpr uint32_t kTaskStackSize = 16 * 1024;
constexpr UBaseType_t kTaskPriority = 4;
constexpr uint32_t kNvsTaskStackSize = 4096;

struct InternalNvsTaskContext {
    std::function<void()> operation;
    SemaphoreHandle_t completed = nullptr;
};

void InternalNvsTaskEntry(void* argument) {
    auto* context = static_cast<InternalNvsTaskContext*>(argument);
    context->operation();
    xSemaphoreGive(context->completed);
    vTaskDelete(nullptr);
}

// NVS 写入期间 ESP-IDF 会关闭 Flash Cache。PSRAM 栈不能在这个阶段被访问，
// 因此把 Settings 的读写放进一个使用内部 RAM 栈的短任务中执行。
bool RunOnInternalNvsStack(std::function<void()> operation) {
    auto context = std::make_unique<InternalNvsTaskContext>();
    context->operation = std::move(operation);
    context->completed = xSemaphoreCreateBinary();
    if (context->completed == nullptr) {
        ESP_LOGE(kTag, "创建 NVS 同步信号量失败");
        return false;
    }

    auto* raw_context = context.release();
    if (xTaskCreate(InternalNvsTaskEntry, "sd_nvs", kNvsTaskStackSize,
                    raw_context, kTaskPriority, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "创建 NVS 内部栈任务失败");
        vSemaphoreDelete(raw_context->completed);
        delete raw_context;
        return false;
    }

    xSemaphoreTake(raw_context->completed, portMAX_DELAY);
    vSemaphoreDelete(raw_context->completed);
    delete raw_context;
    return true;
}

std::string BuildStorageStamp(const SdCardInfo& info) {
    // 扫描目录或编码规则变化时强制重新上报，避免继续使用旧策略生成的文件列表。
    return "scan-audio-only-v3:encoding-v2:free:" +
           std::to_string(info.free_bytes);
}

std::string BuildScanId() {
    static std::atomic_uint64_t last_id{0};
    uint64_t value = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    uint64_t previous = last_id.load();
    while (value <= previous ||
           !last_id.compare_exchange_weak(previous, value)) {
        if (value <= previous) value = previous + 1;
    }
    return "scan-" + std::to_string(value);
}

class FileAccessGuard {
public:
    explicit FileAccessGuard(std::atomic_bool& active) : active_(active) {
        active_.store(true);
    }
    ~FileAccessGuard() { active_.store(false); }

private:
    std::atomic_bool& active_;
};

}  // namespace

struct SdMusicService::WorkerContext {
    SdMusicService* service = nullptr;
    uint32_t generation = 0;
    std::string authorization;
    bool external_stack = false;
};

struct SdMusicService::RemovedContext {
    std::string card_id;
    std::string authorization;
    bool external_stack = false;
};

SdMusicService& SdMusicService::GetInstance() {
    static SdMusicService service;
    return service;
}

void SdMusicService::Initialize(AuthorizationProvider authorization_provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    authorization_provider_ = std::move(authorization_provider);
    initialized_ = true;
}

void SdMusicService::OnCardInserted() {
    SdCardInfo info;
    if (SdCardManager::GetInstance().GetCardInfo(info) != ESP_OK ||
        info.card_id.empty()) {
        ESP_LOGW(kTag, "TF 卡已挂载，但无法读取卡信息");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_card_id_ = info.card_id;
        pending_sync_ = true;
    }
    cancelled_.store(false);
    card_ready_.store(true);
    generation_.fetch_add(1);
    // TF 图标还要等待小聆长连接就绪；连接已经建立时，热插卡立即显示。
    Ui::SetTfCardPresent(cloud_ready_.load());
    TryStartSync();
}

void SdMusicService::OnCardRemoved() {
    std::string card_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        card_id = current_card_id_;
        current_card_id_.clear();
        pending_sync_ = false;
    }
    cancelled_.store(true);
    generation_.fetch_add(1);
    busy_.store(false);
    card_ready_.store(false);
    {
        Settings settings(kSettingsNamespace, true);
        settings.EraseKey(kPresentCardKey);
    }
    Ui::SetTfCardPresent(false);
    Ui::DismissSdMusicSync();
    Ui::ShowToast("设备已弹出TF卡", 3000);
    ReportRemovedAsync(std::move(card_id));
}

void SdMusicService::OnUnsupportedFileSystem() {
    Ui::SetTfCardPresent(false);
    PublishState(SdMusicSyncState{
        .phase = SdMusicSyncPhase::kFailed,
        .result = SdMusicSyncResult::kUnsupportedFileSystem,
    });
}

void SdMusicService::OnCloudReady() {
    cloud_ready_.store(true);
    Ui::SetTfCardPresent(card_ready_.load());

    AuthorizationProvider provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider = authorization_provider_;
    }
    const std::string authorization = provider ? provider() : std::string{};
    if (!authorization.empty()) {
        ReconcileBootRemoval(authorization);
    }
    TryStartSync();
}

void SdMusicService::OnCloudDisconnected() {
    cloud_ready_.store(false);
    Ui::SetTfCardPresent(false);
}

bool SdMusicService::ClearFingerprint() {
    const bool cleared = RunOnInternalNvsStack([] {
        Settings settings(kSettingsNamespace, true);
        settings.EraseKey(kCardIdKey);
        settings.EraseKey(kStorageStampKey);
        settings.EraseKey(kPresentCardKey);
    });
    if (!cleared) {
        ESP_LOGE(kTag, "清除 TF 卡同步指纹失败");
        return false;
    }

    bool card_ready = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        card_ready = card_ready_.load();
        pending_sync_ = card_ready;
    }
    if (card_ready) {
        // 让正在执行的同步任务尽快退出，FinishWorker 会启动本次强制同步。
        cancelled_.store(true);
        generation_.fetch_add(1);
        cancelled_.store(false);
        TryStartSync();
    }
    ESP_LOGI(kTag, "TF 卡同步指纹已清除%s",
             card_ready ? "，当前卡将在鉴权就绪后重新同步" : "");
    return true;
}

void SdMusicService::TryStartSync() {
    AuthorizationProvider provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || !pending_sync_ || worker_task_ != nullptr ||
            !card_ready_.load()) {
            return;
        }
        provider = authorization_provider_;
    }
    const std::string authorization = provider ? provider() : std::string{};
    if (authorization.empty()) {
        ESP_LOGI(kTag, "等待小聆鉴权完成后同步 TF 卡");
        return;
    }

    auto context = std::make_unique<WorkerContext>();
    context->service = this;
    context->generation = generation_.load();
    context->authorization = authorization;
    TaskHandle_t task = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_sync_ || worker_task_ != nullptr ||
            !card_ready_.load()) {
            return;
        }
        // 持锁创建并登记句柄，避免新任务先运行完毕后留下失效句柄。
        if (PsramTask::Create(SyncTaskEntry, "sd_music_sync", kTaskStackSize,
                              context.get(), kTaskPriority, &task,
                              &context->external_stack) != pdPASS) {
            ESP_LOGE(kTag, "创建 TF 卡同步任务失败");
            return;
        }
        worker_task_ = task;
        pending_sync_ = false;
    }
    context.release();
}

void SdMusicService::SyncTaskEntry(void* arg) {
    std::unique_ptr<WorkerContext> context(static_cast<WorkerContext*>(arg));
    const bool external_stack = context->external_stack;
    context->service->RunSync(context->generation, context->authorization);
    context->service->FinishWorker(context->generation);
    context.reset();
    PsramTask::Delete(external_stack);
}

bool SdMusicService::IsCurrent(uint32_t generation) const {
    return generation_.load() == generation && !cancelled_.load() &&
           card_ready_.load();
}

void SdMusicService::RunSync(uint32_t generation,
                             const std::string& authorization) {
    busy_.store(true);
    PublishState(SdMusicSyncState{.phase = SdMusicSyncPhase::kScanning});

    SdCardInfo info;
    if (!IsCurrent(generation) ||
        SdCardManager::GetInstance().GetCardInfo(info) != ESP_OK) {
        busy_.store(false);
        return;
    }
    const std::string storage_stamp = BuildStorageStamp(info);
    SdMusicHttp http(authorization);
    const SdMusicHttpResult inserted =
        http.ReportCardEvent(info.card_id, "inserted", info.capacity_bytes);
    if (!IsCurrent(generation)) {
        busy_.store(false);
        return;
    }
    if (inserted.error != ESP_OK) {
        PublishState(SdMusicSyncState{
            .phase = SdMusicSyncPhase::kFailed,
            .result = SdMusicSyncResult::kFailed,
            .http_status_code = static_cast<uint32_t>(inserted.status_code),
            .http_error_code = inserted.error,
        });
        busy_.store(false);
        return;
    }
    {
        const std::string card_id = info.card_id;
        RunOnInternalNvsStack([card_id]() {
            Settings settings(kSettingsNamespace, true);
            settings.SetString(kPresentCardKey, card_id);
        });
    }

    bool unchanged = false;
    {
        const std::string card_id = info.card_id;
        const std::string stamp = storage_stamp;
        RunOnInternalNvsStack([&unchanged, card_id, stamp]() {
            Settings settings(kSettingsNamespace);
            unchanged = settings.GetString(kCardIdKey) == card_id &&
                        settings.GetString(kStorageStampKey) == stamp;
        });
    }
    if (unchanged) {
        ESP_LOGI(kTag, "TF 卡存储指纹未变化，跳过文件列表上报: %s",
                 storage_stamp.c_str());
        PublishState(SdMusicSyncState{
            .phase = SdMusicSyncPhase::kSucceeded,
            .result = SdMusicSyncResult::kDataUnchanged,
        });
        busy_.store(false);
        return;
    }

    SdMusicScanStats count_stats;
    esp_err_t scan_result = ESP_OK;
    {
        FileAccessGuard guard(file_access_active_);
        scan_result = SdMusicScanner::Count({}, &count_stats, &cancelled_);
    }
    if (!IsCurrent(generation)) {
        busy_.store(false);
        return;
    }
    if (scan_result != ESP_OK) {
        PublishState(SdMusicSyncState{
            .phase = SdMusicSyncPhase::kFailed,
            .result = SdMusicSyncResult::kFailed,
            .http_error_code = scan_result,
        });
        busy_.store(false);
        return;
    }

    const std::string scan_id = BuildScanId();
    uint32_t uploaded_count = 0;
    SdMusicHttpResult upload_result;
    PublishState(SdMusicSyncState{
        .phase = SdMusicSyncPhase::kUploading,
        .total_count = count_stats.reported_count,
        .skipped_count = count_stats.skipped_count,
    });
    SdMusicScanStats upload_stats;
    {
        FileAccessGuard guard(file_access_active_);
        scan_result = SdMusicScanner::Scan(
            {}, 0, true,
            [&](const std::vector<SdMusicTrack>& tracks, bool is_last) {
                if (!IsCurrent(generation)) return ESP_ERR_INVALID_STATE;
                upload_result = http.ReportFiles(info.card_id, scan_id, tracks,
                                                 is_last);
                if (upload_result.error != ESP_OK) return upload_result.error;
                uploaded_count += tracks.size();
                PublishState(SdMusicSyncState{
                    .phase = SdMusicSyncPhase::kUploading,
                    .uploaded_count = uploaded_count,
                    .total_count = count_stats.reported_count,
                    .skipped_count = count_stats.skipped_count,
                });
                return ESP_OK;
            },
            &upload_stats, &cancelled_);
    }
    if (!IsCurrent(generation)) {
        busy_.store(false);
        return;
    }
    if (scan_result == ESP_OK) {
        const std::string card_id = info.card_id;
        const std::string stamp = storage_stamp;
        RunOnInternalNvsStack([card_id, stamp]() {
            Settings settings(kSettingsNamespace, true);
            settings.SetString(kCardIdKey, card_id);
            settings.SetString(kStorageStampKey, stamp);
            settings.SetString(kPresentCardKey, card_id);
        });
        ESP_LOGI(kTag,
                 "TF 卡文件列表同步完成: scan_id=%s, uploaded=%lu, skipped=%lu",
                 scan_id.c_str(), static_cast<unsigned long>(uploaded_count),
                 static_cast<unsigned long>(count_stats.skipped_count));
        PublishState(SdMusicSyncState{
            .phase = SdMusicSyncPhase::kSucceeded,
            .uploaded_count = uploaded_count,
            .total_count = count_stats.reported_count,
            .result = SdMusicSyncResult::kOk,
            .skipped_count = count_stats.skipped_count,
        });
    } else {
        PublishState(SdMusicSyncState{
            .phase = SdMusicSyncPhase::kFailed,
            .uploaded_count = uploaded_count,
            .total_count = count_stats.reported_count,
            .result = SdMusicSyncResult::kFailed,
            .http_status_code = static_cast<uint32_t>(upload_result.status_code),
            .http_error_code = upload_result.error != ESP_OK
                                   ? upload_result.error
                                   : scan_result,
            .skipped_count = count_stats.skipped_count,
        });
    }
    busy_.store(false);
}

void SdMusicService::FinishWorker(uint32_t generation) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_task_ = nullptr;
    }
    if (generation_.load() != generation) {
        TryStartSync();
    }
}

void SdMusicService::PublishState(const SdMusicSyncState& state) const {
    Ui::ShowSdMusicSync(state);
}

bool SdMusicService::WaitForFileAccessToStop(uint32_t timeout_ms) const {
    const TickType_t period = pdMS_TO_TICKS(20);
    const uint32_t polls = std::max<uint32_t>(1, timeout_ms / 20);
    for (uint32_t i = 0; i < polls; ++i) {
        if (!file_access_active_.load()) return true;
        vTaskDelay(period);
    }
    return !file_access_active_.load();
}

void SdMusicService::ReportRemovedAsync(std::string card_id) {
    if (card_id.empty()) return;
    AuthorizationProvider provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider = authorization_provider_;
    }
    const std::string authorization = provider ? provider() : std::string{};
    if (authorization.empty()) {
        ESP_LOGW(kTag, "没有可用鉴权信息，跳过 removed 事件上报");
        return;
    }
    auto context = std::make_unique<RemovedContext>();
    context->card_id = std::move(card_id);
    context->authorization = authorization;
    if (PsramTask::Create(RemovedTaskEntry, "sd_removed", 8192,
                          context.get(), kTaskPriority, nullptr,
                          &context->external_stack) != pdPASS) {
        ESP_LOGW(kTag, "创建 removed 上报任务失败");
        return;
    }
    context.release();
}

void SdMusicService::RemovedTaskEntry(void* arg) {
    std::unique_ptr<RemovedContext> context(static_cast<RemovedContext*>(arg));
    const bool external_stack = context->external_stack;
    SdMusicHttp http(context->authorization);
    const auto result = http.ReportCardEvent(context->card_id, "removed", 0);
    if (result.error != ESP_OK) {
        ESP_LOGW(kTag, "removed 事件上报失败: status=%d, error=%s",
                 result.status_code, esp_err_to_name(result.error));
    }
    context.reset();
    PsramTask::Delete(external_stack);
}

void SdMusicService::ReconcileBootRemoval(const std::string& authorization) {
    std::string previous_card_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (boot_state_reconciled_) return;
        boot_state_reconciled_ = true;
    }
    if (SdCardManager::GetInstance().IsMounted()) return;
    {
        Settings settings(kSettingsNamespace, true);
        previous_card_id = settings.GetString(kPresentCardKey);
        if (!previous_card_id.empty()) settings.EraseKey(kPresentCardKey);
    }
    if (previous_card_id.empty()) return;
    ESP_LOGI(kTag, "上电未检测到上次在位的 TF 卡，补报 removed 事件");
    auto context = std::make_unique<RemovedContext>();
    context->card_id = std::move(previous_card_id);
    context->authorization = authorization;
    if (PsramTask::Create(RemovedTaskEntry, "sd_boot_removed", 8192,
                          context.get(), kTaskPriority, nullptr,
                          &context->external_stack) == pdPASS) {
        context.release();
    }
}
