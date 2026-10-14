#pragma once

#include "player_channel.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <mutex>

class TonePlayer final : public PlayerChannel {
public:
    using SessionAllocator = std::function<uint32_t()>;

    TonePlayer(PlaybackBackend& engine, SessionAllocator allocate_session);

    PlaybackChannel Type() const override { return PlaybackChannel::kPrompt; }
    uint32_t Submit(const PlaybackRequest& request, PlaybackBackend& engine) override;
    void Cancel(PlaybackBackend& engine) override;
    void OnEvent(const PlaybackEvent& event) override;

    uint32_t PlayTone(uint16_t tone_id);
    void PlayWakeup(std::function<void(bool)> completion);
    void CancelWakeup();
    bool IsWakeupPending() const {
        std::lock_guard<std::mutex> lock(wakeup_mutex_);
        return static_cast<bool>(completion_);
    }
    uint32_t WakeupSessionId() const { return wakeup_session_id_.load(); }
    bool IsWakeupTerminalEvent(const PlaybackEvent& event) const;

private:
    std::optional<uint16_t> SelectWakeupTone();
    void CompleteWakeup(bool proceed, uint32_t expected_session = 0);

    PlaybackBackend* engine_;
    SessionAllocator allocate_session_;
    std::optional<uint16_t> last_wakeup_tone_;
    std::atomic<uint32_t> wakeup_session_id_{0};
    std::function<void(bool)> completion_;
    mutable std::mutex wakeup_mutex_;
};
