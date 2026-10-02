#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace AppEvent {

enum class Id : uint16_t {
    None = 0,

    // 播放器内部事件；后续阶段由播放器独占订阅。
    PlaybackBackend,

    // 对外发布的播放器生命周期事件。
    PlaybackStreamReady,
    PlaybackStarted,
    PlaybackPaused,
    PlaybackStopped,
    PlaybackCompleted,
    PlaybackFailed,

    // 音频输入事件由 AudioService 发布；播放器输出事件由 AudioPlayer 发布。
    WakeWordDetected,
    VadChanged,
    PlayerOutputStarted,
    PlayerOutputProgress,
};

enum class Priority : uint8_t {
    Normal,
    Urgent,
};

struct Event {
    Id id = Id::None;
    const void* data = nullptr;
    size_t size = 0;

    template <typename T>
    const T* Get() const {
        static_assert(std::is_trivially_copyable_v<T>,
                      "应用事件只直接承载可复制的结构体");
        return size == sizeof(T) && data != nullptr
                   ? static_cast<const T*>(data)
                   : nullptr;
    }
};

using Handler = void (*)(const Event& event, void* context);
using SubscriptionId = uint32_t;

/**
 * 创建应用事件任务和队列。重复调用不会创建第二个任务。
 */
bool Initialize();

/**
 * 订阅指定事件。回调统一在 app_event 任务中串行执行。
 */
SubscriptionId Subscribe(Id id, Handler handler, void* context = nullptr);

/**
 * 取消订阅。当前阶段订阅者均为进程级对象，主要用于完整生命周期管理。
 */
void Unsubscribe(SubscriptionId subscription_id);

/**
 * 复制 payload 后异步发布事件。本接口不能在中断上下文调用。
 */
bool Publish(Id id, const void* data = nullptr, size_t size = 0,
             Priority priority = Priority::Normal);

template <typename T>
bool Publish(Id id, const T& value,
             Priority priority = Priority::Normal) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "应用事件只直接承载可复制的结构体");
    return Publish(id, &value, sizeof(T), priority);
}

}  // namespace AppEvent
