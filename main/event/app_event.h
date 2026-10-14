#pragma once

#include "event_epoch.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
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

    // 小聆连接、IAT 和 TTS 事件；文本内容通过 PublishText() 复制传递。
    XiaolingConnected,
    XiaolingDisconnected,
    XiaolingIatStart,
    XiaolingIatUpdate,
    XiaolingIatEnd,
    XiaolingTtsUrl,
    XiaolingPushTtsUrl,
    XiaolingTtsTextStart,
    XiaolingTtsTextUpdate,
    XiaolingTtsTextEnd,
    XiaolingTtsTextInterrupt,
    XiaolingTtsStreamReady,
    XiaolingTtsPlaying,
    XiaolingTtsStarted,
    XiaolingTtsOutputProgress,
    XiaolingTtsStopped,
    VoiceSessionStarted,
};

enum class Priority : uint8_t {
    Normal,
    Urgent,
};

struct Event {
    Id id = Id::None;
    const void* data = nullptr;
    size_t size = 0;
    const Epoch* epoch = nullptr;
    uint32_t generation = 0;

    bool IsCurrent() const {
        return epoch == nullptr || epoch->IsCurrent(generation);
    }
    EpochSnapshot Scope() const { return {epoch, generation}; }

    template <typename T>
    const T* Get() const {
        static_assert(std::is_trivially_copyable_v<T>,
                      "应用事件只直接承载可复制的结构体");
        return size == sizeof(T) && data != nullptr
                   ? static_cast<const T*>(data)
                   : nullptr;
    }

    // 仅用于 PublishText() 发布的事件；返回值只在当前回调期间有效。
    std::string_view Text() const {
        return data == nullptr ? std::string_view{}
                               : std::string_view(static_cast<const char*>(data), size);
    }
};

using Handler = void (*)(const Event& event, void* context);
using Callback = std::function<void(const Event& event)>;
using SubscriptionId = uint32_t;

/**
 * 创建应用事件任务和队列。重复调用不会创建第二个任务。
 */
bool Initialize();

/**
 * 订阅指定事件。回调统一在 app_event 任务中串行执行。
 */
SubscriptionId Subscribe(Id id, Handler handler, void* context = nullptr);
SubscriptionId Subscribe(Id id, Callback callback);

/**
 * 取消订阅。当前阶段订阅者均为进程级对象，主要用于完整生命周期管理。
 */
void Unsubscribe(SubscriptionId subscription_id);

/**
 * 复制 payload 后异步发布事件。本接口不能在中断上下文调用。
 */
bool Publish(Id id, const void* data = nullptr, size_t size = 0,
             Priority priority = Priority::Normal);

// 复制文本后异步发布；接收方通过 Event::Text() 读取。
bool PublishText(Id id, std::string_view text,
                 Priority priority = Priority::Normal);

// 出队以及每个订阅回调前重新检查代次；二次 Schedule 时也应保存并检查代次。
bool PublishScoped(Id id, const void* data, size_t size,
                   const Epoch& epoch, uint32_t generation,
                   Priority priority = Priority::Normal);
bool PublishTextScoped(Id id, std::string_view text,
                       const Epoch& epoch, uint32_t generation,
                       Priority priority = Priority::Normal);

template <typename T>
bool Publish(Id id, const T& value,
             Priority priority = Priority::Normal) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "应用事件只直接承载可复制的结构体");
    return Publish(id, &value, sizeof(T), priority);
}

}  // namespace AppEvent
