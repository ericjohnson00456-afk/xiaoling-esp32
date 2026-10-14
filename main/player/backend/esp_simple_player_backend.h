#pragma once

#include <atomic>
#include <deque>
#include <condition_variable>
#include <mutex>

#include <esp_audio_simple_player.h>
#include <esp_gmf_io.h>
#include <esp_gmf_io_http.h>
#include <esp_gmf_port.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "playback_backend.h"
#include "decode_yield_policy.h"

class EspSimplePlayerBackend final : public PlaybackBackend {
public:
    EspSimplePlayerBackend() = default;
    ~EspSimplePlayerBackend() override;

    bool Initialize(AudioOutput* audio_output, EventCallback callback) override;
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
    static esp_gmf_err_io_t BudgetAcquire(void* ctx, esp_gmf_payload_t* load,
                                         uint32_t wanted, int wait_ticks);
    static esp_gmf_err_io_t BudgetRelease(void* ctx, esp_gmf_payload_t* load, int wait_ticks);
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
    void InterruptTtsHttpSocket();
    void HandleTtsPcmTimeout();
    static void CommandTaskEntry(void* arg);

    AudioOutput* audio_output_ = nullptr;
    // prev 回调在流水线启动前安装端口代理；每个 backend 只有一条解码流水线。
    port_acquire original_output_acquire_ = nullptr;
    port_release original_output_release_ = nullptr;
    void* original_output_ctx_ = nullptr;
    DecodeYieldPolicy decode_yield_policy_;
    std::atomic_bool low_latency_prompt_{false};
    std::atomic_bool tts_decode_{false};
    // SimplePlayer 的工作任务从 new 到 destroy 常驻；由端口回调获取真实句柄。
    std::atomic<TaskHandle_t> decode_task_handle_{nullptr};
    std::atomic_bool cancelling_decode_{false};
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
    // 停止尚未收到首帧的流式 TTS 时，让 HTTP 读取回调尽快结束空等。
    std::atomic_bool tts_stop_requested_{false};
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
