#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

// Intents are ordered from long-lived background work to short-lived,
// foreground work. Add future event types here and give them a priority.
enum class PlaybackIntent : uint8_t {
    kNone = 0,
    kMusic,
    kVoiceSession,
    kTts,
    kPromptTone,
    kAlarm,
    kCount,
};

class PlaybackIntentStack final {
public:
    struct Handlers {
        std::function<void()> on_enter;
        std::function<void()> on_preempted;
        std::function<void()> on_resumed;
        std::function<void()> on_exit;
    };

    static constexpr size_t kCapacity = 8;

    void SetHandlers(PlaybackIntent intent, Handlers handlers);
    bool Push(PlaybackIntent intent);
    bool Reenter(PlaybackIntent intent);
    bool Pop(PlaybackIntent intent);
    bool Contains(PlaybackIntent intent) const;
    PlaybackIntent Top() const;
    void Clear();

private:
    std::array<PlaybackIntent, kCapacity> intents_{};
    std::array<Handlers, static_cast<size_t>(PlaybackIntent::kCount)> handlers_{};
    std::array<bool, static_cast<size_t>(PlaybackIntent::kCount)> entered_{};
    size_t size_ = 0;
};
