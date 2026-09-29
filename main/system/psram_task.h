#pragma once

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

namespace PsramTask {

// Keep large, temporary network-worker stacks out of scarce internal RAM.
// Falling back preserves support for boards without external RAM.
inline BaseType_t Create(TaskFunction_t entry, const char* name,
                         configSTACK_DEPTH_TYPE stack_size, void* argument,
                         UBaseType_t priority, TaskHandle_t* handle,
                         bool* stack_in_psram) {
    if (stack_in_psram == nullptr) {
        return pdFAIL;
    }

    *stack_in_psram = true;
    BaseType_t result = xTaskCreateWithCaps(
        entry, name, stack_size, argument, priority, handle,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result == pdPASS) {
        return result;
    }

    *stack_in_psram = false;
    return xTaskCreate(entry, name, stack_size, argument, priority, handle);
}

inline void Delete(bool stack_in_psram) {
    if (stack_in_psram) {
        vTaskDeleteWithCaps(nullptr);
    } else {
        vTaskDelete(nullptr);
    }
}

}  // namespace PsramTask
