#pragma once

#include "player_channel.h"

#include <functional>
#include <string>

class TtsPlayer final : public PlayerChannel {
public:
    using SessionAllocator = std::function<uint32_t()>;
    using UrlCallback = std::function<void()>;
    using AdmissionCallback = std::function<bool()>;

    TtsPlayer(PlayerEngine& engine, SessionAllocator allocate_session);
    ~TtsPlayer() override;

    PlaybackChannel Type() const override { return PlaybackChannel::kTts; }
    uint32_t Submit(const PlaybackRequest& request, PlayerEngine& engine) override;
    void Cancel(PlayerEngine& engine) override;

    uint32_t PlayUrl(const std::string& url);
    void SetUrlCallback(UrlCallback callback);
    void SetAdmissionCallback(AdmissionCallback callback);
    static bool IsHttpUrl(const std::string& url);

private:
    uint32_t PlayUrlInternal(const std::string& url, bool notify);

    PlayerEngine* engine_;
    SessionAllocator allocate_session_;
    uint32_t subscription_id_ = 0;
    UrlCallback url_callback_;
    AdmissionCallback admission_callback_;
};
