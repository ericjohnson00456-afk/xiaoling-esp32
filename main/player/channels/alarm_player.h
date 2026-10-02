#pragma once

#include "player_channel.h"

#include <cstdint>
#include <functional>
#include <string>

// 闹钟独占通道播放器。只负责提交音频和维护当前 session，不决定响铃阶段。
class AlarmPlayer final : public PlayerChannel {
public:
    using SessionAllocator = std::function<uint32_t()>;

    AlarmPlayer(PlaybackBackend& engine, SessionAllocator allocate_session);

    PlaybackChannel Type() const override { return PlaybackChannel::kAlarm; }
    uint32_t Submit(const PlaybackRequest& request, PlaybackBackend& engine) override;
    void Cancel(PlaybackBackend& engine) override;
    void OnEvent(const PlaybackEvent& event) override;

    uint32_t PlayTone(uint16_t tone_id);
    uint32_t PlayUrl(const std::string& url);

private:
    PlaybackBackend* engine_;
    SessionAllocator allocate_session_;
    uint32_t session_id_ = 0;
};
