#include "system_info.h"
#include "xiaoling/conn/xiaoling_config.h"
#include "shell/settings.h"

#include <freertos/task.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_ota_ops.h>
#include <algorithm>
#include <cstring>
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_wifi_remote.h"
#endif

#define TAG "SystemInfo"

size_t SystemInfo::GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    return esp_get_minimum_free_heap_size();
}

size_t SystemInfo::GetFreeHeapSize() {
    return esp_get_free_heap_size();
}

std::string SystemInfo::GetMacAddress() {
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_STA, mac);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

std::string SystemInfo::GetChipModelName() {
    return std::string(CONFIG_IDF_TARGET);
}

std::string SystemInfo::GetUserAgent() {
    auto app_desc = esp_app_get_description();
    auto user_agent = std::string(BOARD_NAME "/") + app_desc->version;
    return user_agent;
}

std::string SystemInfo::GetDeviceSerialNumber() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    uint8_t serial_number[33] = {};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK &&
        serial_number[0] != 0) {
        return std::string(reinterpret_cast<char*>(serial_number),
                           strnlen(reinterpret_cast<char*>(serial_number), 32));
    }
#endif
    return {};
}

#ifdef CONFIG_LSPLATFORM
std::string SystemInfo::GetProductId() {
    Settings settings("user", false);
    const std::string stored = settings.GetString("pid");
    if (!stored.empty()) {
        return stored;
    }
#if defined(CONFIG_LSPLATFORM_PRODUCT_ID_BUILTIN)
    return std::string(CONFIG_LSPLATFORM_BUILTIN_PRODUCT_ID);
#elif defined(CONFIG_LSPLATFORM_PRODUCT_ID_CUSTOM)
    return std::string(CONFIG_LSPLATFORM_CUSTOM_PRODUCT_ID);
#endif
}

std::string SystemInfo::GetSecretId() {
    Settings settings("user", false);
    const std::string stored = settings.GetString("sid");
    if (!stored.empty()) {
        return stored;
    }
#if defined(CONFIG_LSPLATFORM_PRODUCT_ID_BUILTIN)
    return std::string(CONFIG_LSPLATFORM_BUILTIN_SECRET_ID);
#elif defined(CONFIG_LSPLATFORM_PRODUCT_ID_CUSTOM)
    return std::string(CONFIG_LSCHAT_SECRET_ID);
#endif
}

std::string SystemInfo::GetCloudDeviceId() {
    std::string device_id = XiaolingConfig::kDeviceId;
    if (!device_id.empty()) {
        return device_id;
    }

    // 平台设备 ID 使用连续十六进制字符，默认由 Wi-Fi MAC 派生。
    device_id = GetMacAddress();
    device_id.erase(std::remove(device_id.begin(), device_id.end(), ':'), device_id.end());
    return device_id;
}
#endif // CONFIG_LSPLATFORM

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
    #define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    //Common return path
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo::PrintTaskList() {
    char buffer[1000];
    vTaskList(buffer);
    ESP_LOGI(TAG, "Task list: \n%s", buffer);
}

void SystemInfo::PrintHeapStats() {
    const size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest_sram = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG,
             "heap: internal=%u, minimum=%u, largest=%u, dma=%u, psram=%u",
             static_cast<unsigned>(free_sram),
             static_cast<unsigned>(min_free_sram),
             static_cast<unsigned>(largest_sram),
             static_cast<unsigned>(free_dma),
             static_cast<unsigned>(free_psram));
}
