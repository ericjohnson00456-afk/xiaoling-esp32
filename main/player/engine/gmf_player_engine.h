#pragma once

#include <mutex>

#include <esp_audio_simple_player.h>

#include "player_engine.h"

class GmfPlayerEngine final : public PlayerEngine {
public:
    GmfPlayerEngine() = default;
    ~GmfPlayerEngine() override;

    bool Initialize(AudioService* audio_service, EventCallback callback) override;
    bool Play(const PlaybackRequest& request) override;
    bool Pause() override;
    bool Resume() override;
    bool Stop(PlaybackEndReason reason) override;
    bool IsPlaying() const override;

private:
    static int OutputCallback(uint8_t* data, int data_size, void* ctx);
    static int EventCallbackAdapter(esp_asp_event_pkt_t* event, void* ctx);

    int HandleOutput(uint8_t* data, int data_size);
    int HandleEvent(esp_asp_event_pkt_t* event);
    void Emit(const PlaybackEvent& event);

    AudioService* audio_service_ = nullptr;
    esp_asp_handle_t handle_ = nullptr;
    EventCallback event_callback_;
    mutable std::mutex mutex_;
    PlaybackRequest current_request_;
    PlaybackEndReason stop_reason_ = PlaybackEndReason::kNone;
    uint32_t playback_started_ms_ = 0;
    uint32_t output_chunks_ = 0;
    bool initialized_ = false;
    bool playing_ = false;
};
