#pragma once

#include <atomic>
#include <deque>
#include <condition_variable>
#include <mutex>

#include <esp_audio_simple_player.h>
#include <esp_gmf_io.h>
#include <esp_gmf_io_http.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "playback_backend.h"

class EspSimplePlayerBackend final : public PlaybackBackend {
public:
    EspSimplePlayerBackend() = default;
    ~EspSimplePlayerBackend() override;

    bool Initialize(AudioService* audio_service, EventCallback callback) override;
    bool Play(const PlaybackRequest& request) override;
    bool Pause() override;
    bool Resume() override;
    bool Stop(PlaybackEndReason reason) override;
    bool IsPlaying() const override;

private:
    enum class CommandType {
        kPlay,
        kPause,
        kResume,
        kStop,
        kShutdown,
    };

    struct Command {
        CommandType type = CommandType::kStop;
        PlaybackRequest request;
        PlaybackEndReason reason = PlaybackEndReason::kNone;
        uint32_t completion_id = 0;
    };

    static int ConfigureInputIo(esp_asp_handle_t handle, void* ctx);
    static int HttpIoEventCallback(http_stream_event_msg_t* event);
    static int OutputCallback(uint8_t* data, int data_size, void* ctx);
    static int EventCallbackAdapter(esp_asp_event_pkt_t* event, void* ctx);

    int HandleOutput(uint8_t* data, int data_size);
    int HandleHttpIoEvent(http_stream_event_msg_t* event);
    int HandleEvent(esp_asp_event_pkt_t* event);
    void Emit(const PlaybackEvent& event);
    bool Enqueue(Command command);
    bool DoPlay(const PlaybackRequest& request);
    bool DoPause();
    bool DoResume();
    bool DoStop(PlaybackEndReason reason);
    void CommandLoop();
    void StopCommandTask();
    void ArmTtsPcmTimeout(uint64_t timeout_us);
    void DisarmTtsPcmTimeout();
    void HandleTtsPcmTimeout();
    static void CommandTaskEntry(void* arg);

    AudioService* audio_service_ = nullptr;
    esp_asp_handle_t handle_ = nullptr;
    EventCallback event_callback_;
    mutable std::mutex mutex_;
    PlaybackRequest current_request_;
    PlaybackEndReason stop_reason_ = PlaybackEndReason::kNone;
    uint32_t playback_started_ms_ = 0;
    std::atomic<uint32_t> output_chunks_{0};
    uint32_t tts_timeout_session_id_ = 0;
    int64_t tts_timeout_started_us_ = 0;
    int64_t last_tts_pcm_us_ = 0;
    bool tts_timeout_stop_pending_ = false;
    bool http_request_ready_emitted_ = false;
    uint32_t tts_http_read_sequence_ = 0;
    std::atomic<void*> tts_http_client_{nullptr};
    bool initialized_ = false;
    bool playing_ = false;
    bool draining_ = false;
    std::mutex command_mutex_;
    std::condition_variable command_cv_;
    std::deque<Command> command_queue_;
    uint32_t next_pause_request_id_ = 0;
    uint32_t completed_pause_request_id_ = 0;
    bool last_pause_result_ = false;
    std::atomic_bool command_task_running_{false};
    std::atomic_bool command_task_finished_{false};
    std::atomic_bool command_pending_playback_{false};
    TaskHandle_t command_task_handle_ = nullptr;
    bool command_task_stack_external_ = false;
    esp_timer_handle_t tts_pcm_timer_ = nullptr;
    std::mutex tts_timer_mutex_;
};
