#pragma once

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// SPI 总线仲裁器。事务保持互斥，同时记录 LCD 等待者；SD 完成一个
// 完整命令后若发现 LCD 正在等待，会主动让出一次调度机会，避免连续读卡饿死刷屏。
class SpiBusArbiter final {
public:
    SpiBusArbiter() {
        semaphore_ = xSemaphoreCreateBinaryStatic(&storage_);
        configASSERT(semaphore_ != nullptr);
        const BaseType_t released = xSemaphoreGive(semaphore_);
        configASSERT(released == pdTRUE);
        (void)released;
    }

    SpiBusArbiter(const SpiBusArbiter&) = delete;
    SpiBusArbiter& operator=(const SpiBusArbiter&) = delete;

    bool LockDisplay() {
        display_waiters_.fetch_add(1);
        const BaseType_t taken = xSemaphoreTake(semaphore_, portMAX_DELAY);
        display_waiters_.fetch_sub(1);
        return taken == pdTRUE;
    }

    bool UnlockDisplayFromIsr(BaseType_t* task_woken) {
        return xSemaphoreGiveFromISR(semaphore_, task_woken) == pdTRUE;
    }

    bool LockStorage() {
        return xSemaphoreTake(semaphore_, portMAX_DELAY) == pdTRUE;
    }

    void UnlockStorage() {
        const BaseType_t released = xSemaphoreGive(semaphore_);
        configASSERT(released == pdTRUE);
        (void)released;
        if (display_waiters_.load() != 0) {
            taskYIELD();
        }
    }

private:
    StaticSemaphore_t storage_ = {};
    SemaphoreHandle_t semaphore_ = nullptr;
    std::atomic_uint32_t display_waiters_{0};
};
