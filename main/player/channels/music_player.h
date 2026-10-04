#pragma once

#include "player_channel.h"

#include <atomic>
#include <functional>
#include <string>

class MusicPlayer final : public PlayerChannel {
public:
    using SessionAllocator = std::function<uint32_t()>;

    MusicPlayer(PlaybackBackend& engine, SessionAllocator allocate_session);

    PlaybackChannel Type() const override { return PlaybackChannel::kMusic; }
    uint32_t Submit(const PlaybackRequest& request, PlaybackBackend& engine) override;
    void Cancel(PlaybackBackend& engine) override;
    void OnEvent(const PlaybackEvent& event) override;

    uint32_t PlayUrl(const std::string& url);
    uint32_t PlayFile(const std::string& path);
    bool Pause();
    bool Resume();
    bool IsPaused() const;
    bool IsAudible() const;
    bool IsLocalFilePlaying() const;

private:
    PlaybackBackend* engine_;
    SessionAllocator allocate_session_;
    std::atomic<uint32_t> session_id_{0};
    std::atomic_bool paused_{false};
    std::atomic_bool local_file_{false};
};
