#pragma once

#include "player_channel.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

class TonePlayer final : public PlayerChannel {
public:
    using SessionAllocator = std::function<uint32_t()>;

    TonePlayer(PlayerEngine& engine, SessionAllocator allocate_session);

    PlaybackChannel Type() const override { return PlaybackChannel::kPrompt; }
    uint32_t Submit(const PlaybackRequest& request, PlayerEngine& engine) override;
    void Cancel(PlayerEngine& engine) override;
    void OnEvent(const PlaybackEvent& event) override;

    uint32_t PlayTone(uint16_t tone_id);
    void PlayWakeup(std::function<void(bool)> completion);
    void CancelWakeup();
    bool IsWakeupPending() const { return static_cast<bool>(completion_); }
    bool IsWakeupTerminalEvent(const PlaybackEvent& event) const;

private:
    std::optional<uint16_t> SelectWakeupTone();
    void CompleteWakeup(bool proceed);

    PlayerEngine* engine_;
    SessionAllocator allocate_session_;
    std::optional<uint16_t> last_wakeup_tone_;
    std::atomic<uint32_t> wakeup_session_id_{0};
    std::function<void(bool)> completion_;
};
