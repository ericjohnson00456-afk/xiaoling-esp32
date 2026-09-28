#pragma once

#include "player_channel.h"

#include <atomic>
#include <functional>
#include <string>

class MusicPlayer final : public PlayerChannel {
public:
    using SessionAllocator = std::function<uint32_t()>;

    MusicPlayer(PlayerEngine& engine, SessionAllocator allocate_session);

    PlaybackChannel Type() const override { return PlaybackChannel::kMusic; }
    uint32_t Submit(const PlaybackRequest& request, PlayerEngine& engine) override;
    void Cancel(PlayerEngine& engine) override;
    void OnEvent(const PlaybackEvent& event) override;

    uint32_t PlayUrl(const std::string& url);
    bool Pause();
    bool Resume();
    bool IsPaused() const;
    bool IsAudible() const;

private:
    PlayerEngine* engine_;
    SessionAllocator allocate_session_;
    std::atomic<uint32_t> session_id_{0};
    std::atomic_bool paused_{false};
};
