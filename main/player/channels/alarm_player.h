#pragma once

#include "player_channel.h"

#include <cstdint>
#include <functional>
#include <string>

// 闹钟独占通道播放器。只负责提交音频和回传播放事件，不决定响铃阶段。
class AlarmPlayer final : public PlayerChannel {
public:
    using SessionAllocator = std::function<uint32_t()>;
    using EventCallback = std::function<void(const PlaybackEvent&)>;

    AlarmPlayer(PlayerEngine& engine, SessionAllocator allocate_session);

    PlaybackChannel Type() const override { return PlaybackChannel::kAlarm; }
    uint32_t Submit(const PlaybackRequest& request, PlayerEngine& engine) override;
    void Cancel(PlayerEngine& engine) override;
    void OnEvent(const PlaybackEvent& event) override;

    uint32_t PlayTone(uint16_t tone_id);
    uint32_t PlayUrl(const std::string& url);
    void SetEventCallback(EventCallback callback);

private:
    PlayerEngine* engine_;
    SessionAllocator allocate_session_;
    EventCallback event_callback_;
    uint32_t session_id_ = 0;
};
