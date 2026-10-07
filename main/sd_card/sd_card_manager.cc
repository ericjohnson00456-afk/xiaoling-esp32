#include "sd_card_manager.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <driver/spi_master.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include "system/spi_bus_arbiter.h"

#define TAG "SdCard"

namespace {

constexpr uint32_t kMonitorPeriodMs = 1000;
constexpr TickType_t kMonitorPeriod = pdMS_TO_TICKS(kMonitorPeriodMs);
constexpr char kMountPoint[] = "/sdcard";

class ScopedLogSilencer {
public:
    ScopedLogSilencer(const char* tag, bool enabled)
        : tag_(tag), enabled_(enabled) {
        if (enabled_) {
            previous_level_ = esp_log_level_get(tag_);
            esp_log_level_set(tag_, ESP_LOG_NONE);
        }
    }

    ~ScopedLogSilencer() {
        if (enabled_) {
            esp_log_level_set(tag_, previous_level_);
        }
    }

private:
    const char* tag_;
    bool enabled_;
    esp_log_level_t previous_level_ = ESP_LOG_NONE;
};

bool EqualsAsciiIgnoreCase(char left, char right) {
    return std::tolower(static_cast<unsigned char>(left)) ==
           std::tolower(static_cast<unsigned char>(right));
}

bool IsFatRecoveryDirectory(const char* name) {
    constexpr char kPrefix[] = "FOUND.";
    constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;
    if (std::strlen(name) != kPrefixLength + 3) {
        return false;
    }
    for (size_t index = 0; index < kPrefixLength; ++index) {
        if (!EqualsAsciiIgnoreCase(name[index], kPrefix[index])) {
            return false;
        }
    }
    return std::isdigit(static_cast<unsigned char>(name[kPrefixLength])) &&
           std::isdigit(static_cast<unsigned char>(name[kPrefixLength + 1])) &&
           std::isdigit(static_cast<unsigned char>(name[kPrefixLength + 2]));
}

bool IsFatRecoveryFragment(const char* name) {
    constexpr char kSuffix[] = ".CHK";
    constexpr size_t kSuffixLength = sizeof(kSuffix) - 1;
    const size_t length = std::strlen(name);
    if (length < kSuffixLength) {
        return false;
    }
    for (size_t index = 0; index < kSuffixLength; ++index) {
        if (!EqualsAsciiIgnoreCase(name[length - kSuffixLength + index],
                                   kSuffix[index])) {
            return false;
        }
    }
    return true;
}

bool IsValidConfig(const SdSpiCardConfig& config) {
    return GPIO_IS_VALID_OUTPUT_GPIO(config.mosi) &&
           GPIO_IS_VALID_GPIO(config.miso) &&
           GPIO_IS_VALID_OUTPUT_GPIO(config.sclk) &&
           GPIO_IS_VALID_OUTPUT_GPIO(config.cs) &&
           (config.power == GPIO_NUM_NC ||
            GPIO_IS_VALID_OUTPUT_GPIO(config.power)) &&
           config.power_on_level <= 1;
}

bool OwnsSpiBus(const SdSpiCardConfig& config) {
    return config.bus_ownership == SdSpiBusOwnership::kManagerOwned;
}

}  // namespace

SdCardManager& SdCardManager::GetInstance() {
    static SdCardManager instance;
    return instance;
}

esp_err_t SdCardManager::Initialize() {
    return Mount(true);
}

esp_err_t SdCardManager::Mount(bool log_details) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.has_value()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (mounted_) {
        return ESP_OK;
    }
    const auto& config = *config_;

    // 未配置卡检测引脚时，后台轮询只能尝试挂载。无卡属于预期结果，
    // 探测期间临时隐藏底层驱动错误，避免每秒刷屏；命令触发的挂载保留完整日志。
    const ScopedLogSilencer gpio_log("gpio", !log_details);
    const ScopedLogSilencer fat_log("vfs_fat_sdmmc", !log_details);
    const ScopedLogSilencer sdspi_log("sdspi_transaction", !log_details);

    esp_err_t ret = ESP_OK;
    if (config.power != GPIO_NUM_NC) {
        gpio_config_t power_config = {};
        power_config.pin_bit_mask = 1ULL << config.power;
        power_config.mode = GPIO_MODE_OUTPUT;
        ret = gpio_config(&power_config);
        if (ret != ESP_OK) {
            if (log_details) {
                ESP_LOGE(TAG, "配置 SD 卡电源 GPIO 失败: %s",
                         esp_err_to_name(ret));
            }
            return ret;
        }
        ret = gpio_set_level(config.power, config.power_on_level);
        if (ret != ESP_OK) {
            gpio_reset_pin(config.power);
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (OwnsSpiBus(config)) {
        spi_bus_config_t bus_config = {};
        bus_config.mosi_io_num = config.mosi;
        bus_config.miso_io_num = config.miso;
        bus_config.sclk_io_num = config.sclk;
        bus_config.quadwp_io_num = GPIO_NUM_NC;
        bus_config.quadhd_io_num = GPIO_NUM_NC;
        bus_config.max_transfer_sz = 16 * 1024;

        ret = spi_bus_initialize(config.host, &bus_config, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            if (log_details) {
                ESP_LOGE(TAG, "初始化 SD 卡 SPI 总线失败: %s",
                         esp_err_to_name(ret));
            }
            if (config.power != GPIO_NUM_NC) {
                gpio_set_level(config.power, !config.power_on_level);
                gpio_reset_pin(config.power);
            }
            return ret;
        }
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    if (config.spi_bus_arbiter != nullptr) {
        host.do_transaction = &SdCardManager::DoArbitratedSpiTransaction;
    }
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = config.cs;
    slot_config.host_id = config.host;
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
    };

    sdmmc_card_t* card = nullptr;
    host.slot = config.host;
    ret = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot_config,
                                  &mount_config, &card);
    if (ret != ESP_OK) {
        if (log_details) {
            ESP_LOGW(TAG, "挂载 SD 卡失败: %s", esp_err_to_name(ret));
        }
        if (OwnsSpiBus(config)) {
            spi_bus_free(config.host);
        }
        if (config.power != GPIO_NUM_NC) {
            gpio_set_level(config.power, !config.power_on_level);
            gpio_reset_pin(config.power);
        }
        return ret;
    }

    card_ = card;
    mounted_ = true;
    removal_pending_ = false;
    if (log_details) {
        ESP_LOGI(TAG, "SD 卡已挂载: %s", kMountPoint);
        sdmmc_card_print_info(stdout, card);
    }
    return ESP_OK;
}

esp_err_t SdCardManager::DoArbitratedSpiTransaction(
    int slot, sdmmc_command_t* command) {
    auto& manager = GetInstance();
    if (!manager.config_.has_value() ||
        manager.config_->spi_bus_arbiter == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    auto* arbiter = manager.config_->spi_bus_arbiter;
    if (!arbiter->LockStorage()) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t ret = sdspi_host_do_transaction(slot, command);
    arbiter->UnlockStorage();
    return ret;
}

esp_err_t SdCardManager::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.has_value()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!mounted_) {
        return ESP_ERR_INVALID_STATE;
    }
    const auto& config = *config_;

    // 拔卡后释放 SPI 引脚会触发 GPIO 驱动的信息日志，业务层状态日志已足够。
    const ScopedLogSilencer gpio_log("gpio", true);
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(
        kMountPoint, static_cast<sdmmc_card_t*>(card_));
    card_ = nullptr;
    mounted_ = false;
    removal_pending_ = false;
    const esp_err_t bus_ret = OwnsSpiBus(config)
                                  ? spi_bus_free(config.host)
                                  : ESP_OK;
    esp_err_t power_ret = ESP_OK;
    if (config.power != GPIO_NUM_NC) {
        power_ret = gpio_set_level(config.power, !config.power_on_level);
        const esp_err_t reset_ret = gpio_reset_pin(config.power);
        if (power_ret == ESP_OK) {
            power_ret = reset_ret;
        }
    }
    if (ret == ESP_OK) {
        ret = bus_ret;
    }
    if (ret == ESP_OK) {
        ret = power_ret;
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD 卡已卸载");
    } else {
        ESP_LOGW(TAG, "卸载 SD 卡时发生错误: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t SdCardManager::ScanFiles(std::vector<SdCardFile>& files) const {
    std::lock_guard<std::mutex> lock(mutex_);
    files.clear();
    if (!mounted_) {
        return ESP_ERR_INVALID_STATE;
    }

    std::vector<std::string> pending_directories{MountPoint()};
    esp_err_t result = ESP_OK;
    while (!pending_directories.empty()) {
        std::string directory = std::move(pending_directories.back());
        pending_directories.pop_back();
        DIR* dir = opendir(directory.c_str());
        if (dir == nullptr) {
            ESP_LOGW(TAG, "无法打开目录: %s, errno=%d", directory.c_str(), errno);
            result = ESP_FAIL;
            continue;
        }
        while (dirent* entry = readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") == 0 ||
                std::strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (IsFatRecoveryDirectory(entry->d_name) ||
                IsFatRecoveryFragment(entry->d_name)) {
                continue;
            }
            const std::string path = directory + "/" + entry->d_name;
            struct stat info = {};
            if (stat(path.c_str(), &info) != 0) {
                ESP_LOGW(TAG, "无法读取文件信息: %s, errno=%d", path.c_str(), errno);
                result = ESP_FAIL;
                continue;
            }
            if (S_ISDIR(info.st_mode)) {
                pending_directories.push_back(path);
            } else if (S_ISREG(info.st_mode)) {
                files.push_back({path, static_cast<uint64_t>(info.st_size)});
            }
        }
        closedir(dir);
    }
    std::sort(files.begin(), files.end(),
              [](const SdCardFile& left, const SdCardFile& right) {
                  return left.path < right.path;
              });
    return result;
}

esp_err_t SdCardManager::GetCardInfo(SdCardInfo& info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    info = {};
    if (!mounted_ || card_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const auto* card = static_cast<const sdmmc_card_t*>(card_);
    const auto& cid = card->cid;
    // ESP-IDF 对 SD 卡保存的是解码后的 CID。按 CID 字段顺序生成固定的
    // 32 位十六进制标识，供云端稳定识别同一张卡。
    char card_id[33] = {};
    std::snprintf(card_id, sizeof(card_id),
                  "%02x%04x%02x%02x%02x%02x%02x%02x%08x%04x01",
                  static_cast<unsigned>(cid.mfg_id) & 0xff,
                  static_cast<unsigned>(cid.oem_id) & 0xffff,
                  static_cast<unsigned char>(cid.name[0]),
                  static_cast<unsigned char>(cid.name[1]),
                  static_cast<unsigned char>(cid.name[2]),
                  static_cast<unsigned char>(cid.name[3]),
                  static_cast<unsigned char>(cid.name[4]),
                  static_cast<unsigned>(cid.revision) & 0xff,
                  static_cast<unsigned>(cid.serial),
                  static_cast<unsigned>(cid.date) & 0xffff);
    info.card_id = card_id;
    info.capacity_bytes = static_cast<uint64_t>(card->csd.capacity) *
                          card->csd.sector_size;

    const esp_err_t usage_ret = esp_vfs_fat_info(
        kMountPoint, &info.filesystem_bytes, &info.free_bytes);
    if (usage_ret != ESP_OK) {
        ESP_LOGW(TAG, "读取 SD 卡容量失败: %s", esp_err_to_name(usage_ret));
    }
    return usage_ret;
}

esp_err_t SdCardManager::StartMonitoring(const SdSpiCardConfig& config,
                                         EventCallback callback) {
    if (!IsValidConfig(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mounted_) {
            return ESP_ERR_INVALID_STATE;
        }
        config_ = config;
        event_callback_ = std::move(callback);
        monitoring_ = true;
    }

    // 启动监测前先完成一次同步探测，确保上电已有卡能被标记为初始状态，
    // 也让上层在云连接建立前就知道当前是否有卡。后续插拔仍由轮询任务处理。
    const esp_err_t initial_mount = Mount(false);
    if (initial_mount == ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            filesystem_error_notified_ = false;
        }
        ESP_LOGI(TAG, "检测到 SD 卡插入，已挂载到 %s", MountPoint());
        Notify(SdCardEvent::kInserted);
    } else if (initial_mount == ESP_FAIL) {
        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            should_notify = !filesystem_error_notified_;
            filesystem_error_notified_ = true;
        }
        if (should_notify) {
            ESP_LOGW(TAG, "检测到无法挂载的 SD 卡文件系统");
            Notify(SdCardEvent::kUnsupportedFileSystem);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (monitor_task_ == nullptr &&
            xTaskCreate(MonitorTaskEntry, "sd_hotplug", 4096, this,
                        tskIDLE_PRIORITY + 2, &monitor_task_) != pdPASS) {
            monitoring_ = false;
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "SD 卡轮询监测已启动: 周期=%lu ms",
             static_cast<unsigned long>(kMonitorPeriodMs));
    return ESP_OK;
}

void SdCardManager::MonitorTaskEntry(void* arg) {
    static_cast<SdCardManager*>(arg)->MonitorLoop();
}

void SdCardManager::MonitorLoop() {
    while (true) {
        vTaskDelay(kMonitorPeriod);
        if (!monitoring_) {
            continue;
        }

        bool mounted = false;
        bool removal_pending = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mounted = mounted_;
            removal_pending = removal_pending_;
        }
        if (!mounted) {
            const esp_err_t mount_result = Mount(false);
            if (mount_result == ESP_OK) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    filesystem_error_notified_ = false;
                }
                ESP_LOGI(TAG, "检测到 SD 卡插入，已挂载到 %s", MountPoint());
                Notify(SdCardEvent::kInserted);
            } else if (mount_result == ESP_FAIL) {
                bool should_notify = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    should_notify = !filesystem_error_notified_;
                    filesystem_error_notified_ = true;
                }
                if (should_notify) {
                    ESP_LOGW(TAG, "检测到无法挂载的 SD 卡文件系统");
                    Notify(SdCardEvent::kUnsupportedFileSystem);
                }
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                filesystem_error_notified_ = false;
            }
            continue;
        }
        if (removal_pending || ProbeMountedCard()) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!mounted_ || removal_pending_) {
                continue;
            }
            removal_pending_ = true;
        }
        ESP_LOGI(TAG, "检测到 SD 卡拔出");
        Notify(SdCardEvent::kRemoved);
    }
}

bool SdCardManager::ProbeMountedCard() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        return false;
    }
    // 卡被拔出后，FATFS 的状态检查失败是探测结果，不作为底层错误重复输出。
    const ScopedLogSilencer diskio_log("diskio_sdmmc", true);
    DIR* dir = opendir(MountPoint());
    if (dir == nullptr) {
        return false;
    }
    closedir(dir);
    return true;
}

void SdCardManager::Notify(SdCardEvent event) {
    EventCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = event_callback_;
    }
    if (callback) {
        callback(event);
    }
}

bool SdCardManager::IsConfigured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.has_value();
}

bool SdCardManager::IsMounted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mounted_;
}

const char* SdCardManager::MountPoint() const {
    return kMountPoint;
}
