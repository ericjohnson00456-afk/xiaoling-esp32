#include "app_event.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace AppEvent {
namespace {

constexpr char kTag[] = "AppEvent";
constexpr UBaseType_t kTaskPriority = 6;
constexpr uint32_t kTaskStackSize = 8 * 1024;
constexpr size_t kNormalQueueSize = 32;
constexpr size_t kUrgentQueueSize = 8;
constexpr size_t kMaxSubscriptions = 64;
constexpr size_t kMaxPayloadSize = 16 * 1024;
constexpr int64_t kSlowHandlerUs = 20 * 1000;

struct QueuedEvent {
    Id id;
    size_t size;
    const Epoch* epoch;
    uint32_t generation;
    alignas(std::max_align_t) uint8_t data[1];
};

struct Subscription {
    SubscriptionId id = 0;
    Id event_id = Id::None;
    Callback callback;
};

std::mutex subscription_mutex;
std::array<Subscription, kMaxSubscriptions> subscriptions;
SubscriptionId next_subscription_id = 1;
QueueHandle_t normal_queue = nullptr;
QueueHandle_t urgent_queue = nullptr;
TaskHandle_t task_handle = nullptr;

QueuedEvent* AllocateEvent(Id id, const void* data, size_t size,
                           const Epoch* epoch, uint32_t generation) {
    const size_t allocation_size = offsetof(QueuedEvent, data) + (size == 0 ? 1 : size);
    auto* queued = static_cast<QueuedEvent*>(std::malloc(allocation_size));
    if (queued == nullptr) {
        return nullptr;
    }
    queued->id = id;
    queued->size = size;
    queued->epoch = epoch;
    queued->generation = generation;
    if (size > 0) {
        std::memcpy(queued->data, data, size);
    }
    return queued;
}

void Dispatch(const QueuedEvent& queued) {
    if (queued.epoch != nullptr && !queued.epoch->IsCurrent(queued.generation)) {
        return;
    }
    std::array<Subscription, kMaxSubscriptions> matching{};
    size_t matching_count = 0;
    {
        std::lock_guard<std::mutex> lock(subscription_mutex);
        for (const auto& subscription : subscriptions) {
            if (subscription.id != 0 && subscription.event_id == queued.id) {
                matching[matching_count++] = subscription;
            }
        }
    }

    const Event event{
        .id = queued.id,
        .data = queued.size == 0 ? nullptr : queued.data,
        .size = queued.size,
        .epoch = queued.epoch,
        .generation = queued.generation,
    };
    for (size_t i = 0; i < matching_count; ++i) {
        if (!event.IsCurrent()) return;
        const int64_t started_us = esp_timer_get_time();
        matching[i].callback(event);
        const int64_t elapsed_us = esp_timer_get_time() - started_us;
        if (elapsed_us >= kSlowHandlerUs) {
            ESP_LOGW(kTag,
                     "事件回调耗时过长: event=%u, subscription=%lu, elapsed=%lu us",
                     static_cast<unsigned>(queued.id),
                     static_cast<unsigned long>(matching[i].id),
                     static_cast<unsigned long>(elapsed_us));
        }
    }
}

bool ReceiveNext(QueuedEvent*& event) {
    if (xQueueReceive(urgent_queue, &event, 0) == pdPASS) {
        return true;
    }
    return xQueueReceive(normal_queue, &event, 0) == pdPASS;
}

void TaskLoop(void*) {
    ESP_LOGI(kTag,
             "应用事件任务已启动: priority=%u, stack=%u, normal=%u, urgent=%u",
             static_cast<unsigned>(kTaskPriority),
             static_cast<unsigned>(kTaskStackSize),
             static_cast<unsigned>(kNormalQueueSize),
             static_cast<unsigned>(kUrgentQueueSize));
    while (true) {
        QueuedEvent* event = nullptr;
        if (!ReceiveNext(event)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        Dispatch(*event);
        std::free(event);
    }
}

}  // namespace

bool Initialize() {
    if (task_handle != nullptr) {
        return true;
    }

    normal_queue = xQueueCreate(kNormalQueueSize, sizeof(QueuedEvent*));
    urgent_queue = xQueueCreate(kUrgentQueueSize, sizeof(QueuedEvent*));
    if (normal_queue == nullptr || urgent_queue == nullptr) {
        ESP_LOGE(kTag, "创建应用事件队列失败");
        if (normal_queue != nullptr) {
            vQueueDelete(normal_queue);
            normal_queue = nullptr;
        }
        if (urgent_queue != nullptr) {
            vQueueDelete(urgent_queue);
            urgent_queue = nullptr;
        }
        return false;
    }

    if (xTaskCreate(TaskLoop, "app_event", kTaskStackSize, nullptr,
                    kTaskPriority, &task_handle) != pdPASS) {
        ESP_LOGE(kTag, "创建应用事件任务失败");
        vQueueDelete(normal_queue);
        vQueueDelete(urgent_queue);
        normal_queue = nullptr;
        urgent_queue = nullptr;
        task_handle = nullptr;
        return false;
    }
    return true;
}

SubscriptionId Subscribe(Id id, Handler handler, void* context) {
    if (id == Id::None || handler == nullptr) {
        return 0;
    }

    return Subscribe(id, [handler, context](const Event& event) {
        handler(event, context);
    });
}

SubscriptionId Subscribe(Id id, Callback callback) {
    if (id == Id::None || !callback) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(subscription_mutex);
    for (auto& subscription : subscriptions) {
        if (subscription.id == 0) {
            SubscriptionId id_value = next_subscription_id++;
            if (id_value == 0) {
                id_value = next_subscription_id++;
            }
            subscription = Subscription{id_value, id, std::move(callback)};
            return id_value;
        }
    }
    ESP_LOGE(kTag, "事件订阅表已满: event=%u", static_cast<unsigned>(id));
    return 0;
}

void Unsubscribe(SubscriptionId subscription_id) {
    if (subscription_id == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(subscription_mutex);
    for (auto& subscription : subscriptions) {
        if (subscription.id == subscription_id) {
            subscription = {};
            return;
        }
    }
}

static bool PublishImpl(Id id, const void* data, size_t size, Priority priority,
                        const Epoch* epoch, uint32_t generation) {
    if (id == Id::None || (size > 0 && data == nullptr) ||
        size > kMaxPayloadSize) {
        ESP_LOGE(kTag, "拒绝无效事件: event=%u, size=%u",
                 static_cast<unsigned>(id), static_cast<unsigned>(size));
        return false;
    }
    if (task_handle == nullptr || normal_queue == nullptr || urgent_queue == nullptr) {
        ESP_LOGE(kTag, "事件循环尚未初始化: event=%u", static_cast<unsigned>(id));
        return false;
    }

    QueuedEvent* event = AllocateEvent(id, data, size, epoch, generation);
    if (event == nullptr) {
        ESP_LOGE(kTag, "分配事件失败: event=%u, size=%u",
                 static_cast<unsigned>(id), static_cast<unsigned>(size));
        return false;
    }

    QueueHandle_t queue = priority == Priority::Urgent ? urgent_queue : normal_queue;
    if (xQueueSend(queue, &event, 0) != pdPASS) {
        ESP_LOGE(kTag, "事件队列已满: event=%u, priority=%s",
                 static_cast<unsigned>(id),
                 priority == Priority::Urgent ? "urgent" : "normal");
        std::free(event);
        return false;
    }
    xTaskNotifyGive(task_handle);
    return true;
}

bool Publish(Id id, const void* data, size_t size, Priority priority) {
    return PublishImpl(id, data, size, priority, nullptr, 0);
}

bool PublishText(Id id, std::string_view text, Priority priority) {
    return Publish(id, text.data(), text.size(), priority);
}

bool PublishScoped(Id id, const void* data, size_t size,
                   const Epoch& epoch, uint32_t generation, Priority priority) {
    if (!epoch.IsCurrent(generation)) return false;
    return PublishImpl(id, data, size, priority, &epoch, generation);
}

bool PublishTextScoped(Id id, std::string_view text,
                       const Epoch& epoch, uint32_t generation, Priority priority) {
    return PublishScoped(id, text.data(), text.size(), epoch, generation, priority);
}

}  // namespace AppEvent
