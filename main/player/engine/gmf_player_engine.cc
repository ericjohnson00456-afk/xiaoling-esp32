#include "gmf_player_engine.h"

#include "audio_service.h"
#include "../resource/tone_model.h"
#include "xiaoling/conn/network_diagnostics.h"

#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <esp_audio_simple_player_advance.h>
#include <esp_gmf_io_embed_flash.h>
#include <esp_gmf_io_http.h>
#include <esp_gmf_pipeline.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

#include <cstring>
#include <cerrno>
#include <strings.h>
#include <sys/socket.h>
#include <utility>

#define TAG "GmfPlayerEngine"

namespace {

// esp_http_client_read() keeps reading until the requested length is filled.
// An 8 KiB request therefore turns a live MP3 stream into multi-second bursts.
// Keep each network read small, while retaining enough asynchronous buffering
// to absorb short cloud-generation stalls.
// A live TTS response may only have part of the next MP3 frame available.
// The service generates speech in segments and can expose less than one MP3
// frame at a time. Keep reads small so those bytes reach the decoder instead
// of waiting for a larger batch in esp_http_client_read().
// One MP3 frame is 216 bytes for the current 48 kbps/16 kHz stream. Reading
// 1 KiB at a time drains a server burst fast enough to keep the small lwIP
// receive window moving, while the 50 ms body timeout below still releases a
// partial read promptly when fewer bytes are available.
constexpr size_t kStreamingHttpReadSize = 1024;
constexpr size_t kStreamingHttpBufferSize = 32 * 1024;
// esp_http_client_read() otherwise waits for the whole requested length. A
// short body-only timeout makes it return a partial body as soon as a sparse
// live TTS chunk has arrived. Header/connect operations retain the GMF default.
constexpr int kStreamingHttpBodyReadTimeoutMs = 50;
constexpr size_t kPlayerCommandStackSize = 4096;
constexpr UBaseType_t kPlayerCommandPriority = 7;
constexpr UBaseType_t kNetworkStreamPriority = 12;
constexpr UBaseType_t kPlayerDecodePriority = 11;
// Apply the same deadline to initial synthesis and a mid-stream synthesis
// stall. The initial deadline starts when the HTTP stream is submitted.
constexpr uint64_t kTtsFirstPcmTimeoutUs = 5ULL * 1000ULL * 1000ULL;
constexpr uint64_t kTtsPcmStallTimeoutUs = 5ULL * 1000ULL * 1000ULL;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
constexpr BaseType_t kPlayerCommandCore = 1;
#else
constexpr BaseType_t kPlayerCommandCore = tskNO_AFFINITY;
#endif

std::string UrlHostForLog(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return "invalid";
    }
    const auto host_start = scheme_end + 3;
    const auto host_end = url.find_first_of("/?#", host_start);
    return url.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
}

bool IsTtsPcmTimeout(PlaybackEndReason reason) {
    return reason == PlaybackEndReason::kFirstPcmTimeout ||
           reason == PlaybackEndReason::kPcmStallTimeout;
}

}  // namespace

int GmfPlayerEngine::ConfigureInputIo(esp_asp_handle_t handle, void* ctx) {
    auto* engine = static_cast<GmfPlayerEngine*>(ctx);
    if (engine == nullptr) {
        return ESP_GMF_ERR_INVALID_ARG;
    }

    esp_gmf_pipeline_handle_t pipeline = nullptr;
    if (esp_audio_simple_player_get_pipeline(handle, &pipeline) != ESP_GMF_ERR_OK || pipeline == nullptr) {
        return ESP_GMF_ERR_FAIL;
    }
    esp_gmf_io_handle_t input = nullptr;
    if (esp_gmf_pipeline_get_in(pipeline, &input) != ESP_GMF_ERR_OK || input == nullptr) {
        return ESP_GMF_ERR_FAIL;
    }

    const char* input_tag = OBJ_GET_TAG(input);
    const bool is_http = input_tag != nullptr && strcasecmp(input_tag, "io_http") == 0;

    if (is_http) {
        // Configure the cloned pipeline IO before esp_gmf_pipeline_run() opens
        // it and creates its task/data bus. This overrides the generic HTTP
        // defaults only for playback, without patching managed_components.
        auto* io = static_cast<esp_gmf_io_t*>(input);
        // Pull network data before decoder/output tasks consume their buffers.
        // Keep TTS network work away from the core-0 AFE path.
        io->io_cfg.thread.prio = kNetworkStreamPriority;
        io->io_cfg.thread.core = kPlayerCommandCore;
        io->io_cfg.thread.stack_in_ext = true;
        io->io_cfg.buffer_cfg.io_size = kStreamingHttpReadSize;
        io->io_cfg.buffer_cfg.buffer_size = kStreamingHttpBufferSize;
        // 在音频 TCP/TLS 已连接且 GET 已写出之后通知上层。字幕连接可从
        // 此时开始，不会与音频争抢首个 TLS 建连，同时能利用云端生成
        // 首批音频前的等待窗口。
        auto* http_cfg = static_cast<http_io_cfg_t*>(OBJ_GET_CFG(input));
        if (http_cfg != nullptr) {
            http_cfg->user_data = engine;
            const auto callback_ret =
                esp_gmf_io_http_set_event_callback(input, HttpIoEventCallback);
            if (callback_ret != ESP_GMF_ERR_OK) {
                ESP_LOGW(TAG, "TTS HTTP 事件回调配置失败: %d",
                         static_cast<int>(callback_ret));
            }
        }

        ESP_LOGI(TAG,
                 "TTS HTTP 流式输入已配置: read=%u, ringbuffer=%u bytes, "
                 "priority=%u, core=%d",
                 static_cast<unsigned>(kStreamingHttpReadSize),
                 static_cast<unsigned>(kStreamingHttpBufferSize),
                 static_cast<unsigned>(kNetworkStreamPriority),
                 kPlayerCommandCore);
        // Keep the component's acquire_read implementation intact. The hook
        // above only observes request lifecycle and its engine context has the
        // same lifetime as the player handle.
        return ESP_GMF_ERR_OK;
    }

    if (input_tag == nullptr || strcasecmp(input_tag, "io_embed_flash") != 0) {
        return ESP_GMF_ERR_OK;
    }
    auto& tones = ToneModel::GetInstance();
    if (!tones.initialized() || tones.EmbedItemCount() == 0) {
        return ESP_GMF_ERR_OK;
    }
    return esp_gmf_io_embed_flash_set_context(input, tones.EmbedItems(), tones.EmbedItemCount());
}

GmfPlayerEngine::~GmfPlayerEngine() {
    StopCommandTask();
    DisarmTtsPcmTimeout();
    if (tts_pcm_timer_ != nullptr) {
        esp_timer_delete(tts_pcm_timer_);
        tts_pcm_timer_ = nullptr;
    }
    if (handle_ == nullptr) {
        return;
    }
    if (IsPlaying()) {
        DoStop(PlaybackEndReason::kCancelled);
    }
    esp_audio_simple_player_destroy(handle_);
    handle_ = nullptr;
}

bool GmfPlayerEngine::Initialize(AudioService* audio_service, EventCallback callback) {
    if (initialized_) {
        return true;
    }
    if (audio_service == nullptr) {
        return false;
    }

    /* Do not print every HTTP read/decoder probe at 115200 baud. Serial log
       traffic can starve the streaming path during TTS startup. State and
       error logs remain enabled by the component defaults. */
    esp_log_level_set("ESP_GMF_HTTP", ESP_LOG_WARN);
    esp_log_level_set("ESP_GMF_ASMP_DEC", ESP_LOG_WARN);
    esp_log_level_set("ESP_GMF_TASK", ESP_LOG_WARN);
    esp_log_level_set("ESP_GMF_PORT", ESP_LOG_WARN);
    esp_log_level_set("ESP_GMF_BLOCK", ESP_LOG_WARN);
    esp_log_level_set("ESP_GMF_EMBED_FLASH", ESP_LOG_WARN);

    esp_asp_cfg_t config = {};
    config.out.cb = OutputCallback;
    config.out.user_ctx = this;
    // Decode follows the network input task on the secondary core.
    config.task_prio = kPlayerDecodePriority;
    config.task_stack = 8 * 1024;
    config.task_core = 1;
    // The decoder task has an 8 KiB stack. Keeping it in PSRAM preserves
    // scarce internal SRAM for Wi-Fi/lwIP and DMA during three concurrent
    // TLS streams.
    config.task_stack_in_ext = true;
    config.prev = ConfigureInputIo;
    config.prev_ctx = this;

    const auto result = esp_audio_simple_player_new(&config, &handle_);
    if (result != ESP_GMF_ERR_OK || handle_ == nullptr) {
        ESP_LOGE(TAG, "创建 GMF 播放器失败: 0x%x", result);
        handle_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "播放器解码任务优先级: priority=%u, core=%d",
             static_cast<unsigned>(kPlayerDecodePriority), config.task_core);
    if (esp_audio_simple_player_set_event(handle_, EventCallbackAdapter, this) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "注册 GMF 播放事件失败");
        esp_audio_simple_player_destroy(handle_);
        handle_ = nullptr;
        return false;
    }

    audio_service_ = audio_service;
    event_callback_ = std::move(callback);
    command_task_running_ = true;
    command_task_finished_ = false;
    command_task_stack_external_ = true;
    BaseType_t task_result = xTaskCreatePinnedToCoreWithCaps(
        CommandTaskEntry, "player_ctl", kPlayerCommandStackSize, this,
        kPlayerCommandPriority, &command_task_handle_, kPlayerCommandCore,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_result != pdPASS) {
        command_task_stack_external_ = false;
        task_result = xTaskCreatePinnedToCore(
            CommandTaskEntry, "player_ctl", kPlayerCommandStackSize, this,
            kPlayerCommandPriority, &command_task_handle_, kPlayerCommandCore);
    }
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "创建播放器控制任务失败");
        command_task_running_ = false;
        esp_audio_simple_player_destroy(handle_);
        handle_ = nullptr;
        return false;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            static_cast<GmfPlayerEngine*>(arg)->HandleTtsPcmTimeout();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "tts_pcm_timeout",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &tts_pcm_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "创建 TTS PCM 超时定时器失败");
        StopCommandTask();
        esp_audio_simple_player_destroy(handle_);
        handle_ = nullptr;
        return false;
    }
    initialized_ = true;
    ESP_LOGI(TAG, "流式播放器就绪: output=%d Hz/%d ch/%d bit",
             CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE,
             CONFIG_AUDIO_SIMPLE_PLAYER_CH_CVT_DEST,
             CONFIG_AUDIO_SIMPLE_PLAYER_BIT_CVT_DEST_BITS);
    return true;
}

bool GmfPlayerEngine::Play(const PlaybackRequest& request) {
    if (!initialized_ || handle_ == nullptr ||
        (request.source_type != PlaybackSourceType::kUrl && request.source_type != PlaybackSourceType::kRomfs)) {
        return false;
    }

    command_pending_playback_ = true;
    return Enqueue(Command{
        .type = CommandType::kPlay,
        .request = request,
    });
}

bool GmfPlayerEngine::DoPlay(const PlaybackRequest& request) {
    if (IsPlaying() && !DoStop(PlaybackEndReason::kPreempted)) {
        ESP_LOGE(TAG, "无法停止当前播放会话");
        command_pending_playback_ = false;
        return false;
    }
    DisarmTtsPcmTimeout();

    // 新媒体开始前清掉旧下行 Opus 和残留 PCM，物理 I2S 仍由 AudioOutputTask 独占。
    audio_service_->BeginPlayerPlayback(request.channel == PlaybackChannel::kTts);
    playback_started_ms_ = esp_log_timestamp();
    ESP_LOGI(TAG, "开始流式播放: session=%lu, channel=%d, host=%s, url_bytes=%u",
             static_cast<unsigned long>(request.session_id), static_cast<int>(request.channel),
             UrlHostForLog(request.source).c_str(),
             static_cast<unsigned>(request.source.size()));
    const int64_t started_us = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_request_ = request;
        stop_reason_ = PlaybackEndReason::kNone;
        playing_ = true;
        draining_ = false;
        http_request_ready_emitted_ = false;
        tts_http_client_ = nullptr;
        tts_timeout_session_id_ = request.channel == PlaybackChannel::kTts
                                      ? request.session_id
                                      : 0;
        tts_timeout_started_us_ = request.channel == PlaybackChannel::kTts
                                      ? started_us
                                      : 0;
        last_tts_pcm_us_ = 0;
        tts_timeout_stop_pending_ = false;
        tts_http_read_sequence_ = 0;
    }

    output_chunks_ = 0;
    const auto result = esp_audio_simple_player_run(handle_, request.source.c_str(), nullptr);
    if (result == ESP_GMF_ERR_OK) {
        if (request.channel == PlaybackChannel::kTts) {
            ArmTtsPcmTimeout(kTtsFirstPcmTimeoutUs);
        }
        command_pending_playback_ = false;
        ESP_LOGI(TAG, "GMF run 已提交: session=%lu, elapsed=%lums",
                 static_cast<unsigned long>(request.session_id),
                 static_cast<unsigned long>(esp_log_timestamp() - playback_started_ms_));
        return true;
    }

    ESP_LOGE(TAG, "启动流式播放失败: session=%lu, error=0x%x",
             static_cast<unsigned long>(request.session_id), result);
    if (request.source_type == PlaybackSourceType::kUrl) {
        NetworkDiagnostics::LogSnapshot("player_http_start_failed");
    }
    DisarmTtsPcmTimeout();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
        draining_ = false;
        tts_timeout_session_id_ = 0;
        tts_timeout_started_us_ = 0;
        last_tts_pcm_us_ = 0;
        tts_timeout_stop_pending_ = false;
    }
    audio_service_->ClearPlaybackQueue();
    command_pending_playback_ = false;
    Emit(PlaybackEvent{
        .session_id = request.session_id,
        .channel = request.channel,
        .state = PlaybackState::kFailed,
        .reason = PlaybackEndReason::kFailed,
    });
    return false;
}

bool GmfPlayerEngine::Pause() {
    return Enqueue(Command{.type = CommandType::kPause});
}

bool GmfPlayerEngine::DoPause() {
    bool is_tts = false;
    bool paused = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || handle_ == nullptr || !playing_) {
            return false;
        }
        is_tts = current_request_.channel == PlaybackChannel::kTts;
        paused = esp_audio_simple_player_pause(handle_) == ESP_GMF_ERR_OK;
    }
    if (paused && is_tts) {
        DisarmTtsPcmTimeout();
    }
    return paused;
}

bool GmfPlayerEngine::Resume() {
    return Enqueue(Command{.type = CommandType::kResume});
}

bool GmfPlayerEngine::DoResume() {
    bool is_tts = false;
    bool resumed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || handle_ == nullptr || !playing_) {
            return false;
        }
        is_tts = current_request_.channel == PlaybackChannel::kTts;
        resumed = esp_audio_simple_player_resume(handle_) == ESP_GMF_ERR_OK;
        if (resumed && is_tts) {
            const int64_t now_us = esp_timer_get_time();
            if (output_chunks_.load() == 0) {
                tts_timeout_started_us_ = now_us;
            } else {
                last_tts_pcm_us_ = now_us;
            }
            tts_timeout_stop_pending_ = false;
        }
    }
    if (resumed && is_tts) {
        ArmTtsPcmTimeout(output_chunks_.load() == 0
                             ? kTtsFirstPcmTimeoutUs
                             : kTtsPcmStallTimeoutUs);
    }
    return resumed;
}

bool GmfPlayerEngine::Stop(PlaybackEndReason reason) {
    command_pending_playback_ = false;
    return Enqueue(Command{
        .type = CommandType::kStop,
        .reason = reason,
    });
}

bool GmfPlayerEngine::DoStop(PlaybackEndReason reason) {
    DisarmTtsPcmTimeout();
    bool drain_only = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_) {
            if (draining_) {
                draining_ = false;
                drain_only = true;
            } else {
                return true;
            }
        } else {
            stop_reason_ = reason;
        }
    }
    if (drain_only) {
        audio_service_->ClearPlaybackQueue();
        return true;
    }

    const auto result = esp_audio_simple_player_stop(handle_);
    if (result != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "停止播放失败: 0x%x", result);
        return false;
    }
    return true;
}

bool GmfPlayerEngine::IsPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_ || draining_ || command_pending_playback_.load();
}

bool GmfPlayerEngine::Enqueue(Command command) {
    if (!command_task_running_ || command_task_handle_ == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        // Multiple interruption sources can request the same stop. A single
        // pending stop is sufficient and avoids repeatedly entering GMF.
        if (command.type == CommandType::kStop && !command_queue_.empty() &&
            command_queue_.back().type == CommandType::kStop) {
            command_queue_.back().reason = command.reason;
            return true;
        }
        command_queue_.push_back(std::move(command));
    }
    xTaskNotifyGive(command_task_handle_);
    return true;
}

void GmfPlayerEngine::CommandTaskEntry(void* arg) {
    auto* engine = static_cast<GmfPlayerEngine*>(arg);
    engine->CommandLoop();
    const bool external_stack = engine->command_task_stack_external_;
    engine->command_task_finished_ = true;
    if (external_stack) {
        vTaskDeleteWithCaps(nullptr);
    } else {
        vTaskDelete(nullptr);
    }
}

void GmfPlayerEngine::CommandLoop() {
    while (command_task_running_) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (command_task_running_) {
            Command command;
            {
                std::lock_guard<std::mutex> lock(command_mutex_);
                if (command_queue_.empty()) {
                    break;
                }
                command = std::move(command_queue_.front());
                command_queue_.pop_front();
            }
            switch (command.type) {
                case CommandType::kPlay:
                    DoPlay(command.request);
                    break;
                case CommandType::kPause:
                    DoPause();
                    break;
                case CommandType::kResume:
                    DoResume();
                    break;
                case CommandType::kStop:
                    DoStop(command.reason);
                    break;
                case CommandType::kShutdown:
                    DoStop(PlaybackEndReason::kCancelled);
                    command_task_running_ = false;
                    break;
            }
        }
    }
}

void GmfPlayerEngine::StopCommandTask() {
    if (!command_task_running_ || command_task_handle_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        command_queue_.clear();
        command_queue_.push_back(Command{.type = CommandType::kShutdown});
    }
    xTaskNotifyGive(command_task_handle_);
    for (int i = 0; i < 300 && !command_task_finished_.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!command_task_finished_.load()) {
        ESP_LOGE(TAG, "播放器控制任务退出超时");
    } else {
        command_task_handle_ = nullptr;
    }
}

void GmfPlayerEngine::ArmTtsPcmTimeout(uint64_t timeout_us) {
    std::lock_guard<std::mutex> lock(tts_timer_mutex_);
    if (tts_pcm_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(tts_pcm_timer_);
    const esp_err_t result = esp_timer_start_once(tts_pcm_timer_, timeout_us);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "启动 TTS PCM 超时定时器失败: %s", esp_err_to_name(result));
    }
}

void GmfPlayerEngine::DisarmTtsPcmTimeout() {
    std::lock_guard<std::mutex> lock(tts_timer_mutex_);
    if (tts_pcm_timer_ != nullptr) {
        esp_timer_stop(tts_pcm_timer_);
    }
}

void GmfPlayerEngine::HandleTtsPcmTimeout() {
    PlaybackEndReason timeout_reason = PlaybackEndReason::kNone;
    uint32_t session_id = 0;
    uint64_t remaining_us = 0;
    void* http_client = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_ || current_request_.channel != PlaybackChannel::kTts ||
            current_request_.session_id != tts_timeout_session_id_ ||
            tts_timeout_stop_pending_) {
            return;
        }
        const bool received_pcm = output_chunks_.load() != 0;
        int64_t reference_us = received_pcm
                                   ? last_tts_pcm_us_
                                   : tts_timeout_started_us_;
        if (received_pcm) {
            /* Once playback has started, an upstream synthesis pause is not
             * an audible stall while the PCM reserve is still draining. Use
             * the speaker's last completed output block as the deadline so
             * the five-second timeout measures actual silence. */
            const uint32_t last_output_ms =
                audio_service_->GetPlayerOutputLastActivityMs();
            if (last_output_ms != 0) {
                reference_us = static_cast<int64_t>(last_output_ms) * 1000;
            }
        }
        if (reference_us <= 0) {
            return;
        }
        /* Sample time after taking mutex_. A PCM callback can refresh the
         * reference while this timer callback is waiting for the lock; using
         * a timestamp captured before the lock would make the subtraction
         * negative and its unsigned conversion look like a huge timeout. */
        const int64_t now_us = esp_timer_get_time();
        const uint64_t timeout_us = received_pcm
                                        ? kTtsPcmStallTimeoutUs
                                        : kTtsFirstPcmTimeoutUs;
        const uint64_t elapsed_us = now_us >= reference_us
                                        ? static_cast<uint64_t>(now_us - reference_us)
                                        : 0;
        if (elapsed_us < timeout_us) {
            remaining_us = timeout_us - elapsed_us;
        } else {
            timeout_reason = received_pcm
                                 ? PlaybackEndReason::kPcmStallTimeout
                                 : PlaybackEndReason::kFirstPcmTimeout;
            session_id = current_request_.session_id;
            http_client = tts_http_client_.exchange(nullptr);
            tts_timeout_stop_pending_ = true;
        }
    }

    if (remaining_us != 0) {
        ArmTtsPcmTimeout(remaining_us);
        return;
    }

    const unsigned long timeout_ms = static_cast<unsigned long>(
        (timeout_reason == PlaybackEndReason::kFirstPcmTimeout
             ? kTtsFirstPcmTimeoutUs
             : kTtsPcmStallTimeoutUs) / 1000ULL);
    ESP_LOGE(TAG, "TTS PCM 超时: session=%lu, type=%s, timeout=%lums",
             static_cast<unsigned long>(session_id),
             timeout_reason == PlaybackEndReason::kFirstPcmTimeout
                 ? "first_pcm"
                 : "pcm_stall",
             timeout_ms);
    NetworkDiagnostics::LogSnapshot("tts_pcm_timeout");
    if (http_client != nullptr) {
        const int fd = esp_http_client_get_socket(
            static_cast<esp_http_client_handle_t>(http_client));
        if (fd >= 0) {
            errno = 0;
            const int shutdown_result = shutdown(fd, SHUT_RDWR);
            const int shutdown_errno = shutdown_result == 0 ? 0 : errno;
            ESP_LOGW(TAG,
                     "TTS HTTP 超时主动中断 socket: session=%lu, fd=%d, "
                     "result=%d, errno=%d(%s)",
                     static_cast<unsigned long>(session_id), fd,
                     shutdown_result, shutdown_errno,
                     std::strerror(shutdown_errno));
        } else {
            ESP_LOGW(TAG, "TTS HTTP 超时时 socket 已关闭: session=%lu",
                     static_cast<unsigned long>(session_id));
        }
    }
    if (!Stop(timeout_reason)) {
        ESP_LOGE(TAG, "提交 TTS PCM 超时停止命令失败: session=%lu",
                 static_cast<unsigned long>(session_id));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tts_timeout_stop_pending_ = false;
        }
        ArmTtsPcmTimeout(timeout_reason == PlaybackEndReason::kFirstPcmTimeout
                             ? kTtsFirstPcmTimeoutUs
                             : kTtsPcmStallTimeoutUs);
    }
}

int GmfPlayerEngine::OutputCallback(uint8_t* data, int data_size, void* ctx) {
    return static_cast<GmfPlayerEngine*>(ctx)->HandleOutput(data, data_size);
}

int GmfPlayerEngine::HttpIoEventCallback(http_stream_event_msg_t* event) {
    if (event == nullptr || event->user_data == nullptr) {
        return ESP_GMF_ERR_OK;
    }
    return static_cast<GmfPlayerEngine*>(event->user_data)->HandleHttpIoEvent(event);
}

int GmfPlayerEngine::HandleHttpIoEvent(http_stream_event_msg_t* event) {
    if (event == nullptr) {
        return ESP_GMF_ERR_OK;
    }

    if (event->event_id == HTTP_STREAM_PRE_REQUEST) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playing_ && current_request_.channel == PlaybackChannel::kTts) {
            // Keep the handle so the five-second timeout can unblock a read
            // that is waiting inside esp_http_client.
            tts_http_client_ = event->http_client;
        }
        return ESP_GMF_ERR_OK;
    }
    if (event->event_id == HTTP_STREAM_ON_RESPONSE) {
        uint32_t session_id = 0;
        uint32_t read_sequence = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!playing_ || current_request_.channel != PlaybackChannel::kTts ||
                event->buffer == nullptr || event->buffer_len <= 0) {
                return ESP_GMF_ERR_OK;
            }
            session_id = current_request_.session_id;
            read_sequence = ++tts_http_read_sequence_;
        }

        // A positive return tells GMF that the callback filled its buffer.
        // Retry short body timeouts until data arrives, EOF is reached, the
        // request is cancelled, or the common five-second deadline expires.
        const auto client = static_cast<esp_http_client_handle_t>(event->http_client);
        const int64_t started_us = esp_timer_get_time();

        if (read_sequence == 1) {
            const esp_err_t timeout_result = esp_http_client_set_timeout_ms(
                client, kStreamingHttpBodyReadTimeoutMs);
            if (timeout_result != ESP_OK) {
                ESP_LOGW(TAG, "TTS HTTP 正文短超时设置失败: %s",
                         esp_err_to_name(timeout_result));
            }
            ESP_LOGI(TAG,
                     "TTS HTTP 响应就绪: session=%lu, status=%d, content_length=%ld, "
                     "chunked=%d, body_timeout=%dms",
                     static_cast<unsigned long>(session_id),
                     esp_http_client_get_status_code(client),
                     static_cast<long>(esp_http_client_get_content_length(client)),
                     esp_http_client_is_chunked_response(client) ? 1 : 0,
                     kStreamingHttpBodyReadTimeoutMs);
        }

        int read_len = 0;
        int read_errno = 0;
        uint32_t eagain_retries = 0;
        do {
            errno = 0;
            read_len = esp_http_client_read(
                client, static_cast<char*>(event->buffer), event->buffer_len);
            read_errno = errno;
            if (read_len != -ESP_ERR_HTTP_EAGAIN) {
                break;
            }
            ++eagain_retries;

            bool request_active = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                request_active = playing_ &&
                    current_request_.channel == PlaybackChannel::kTts &&
                    current_request_.session_id == session_id;
            }
            if (!request_active ||
                esp_timer_get_time() - started_us >=
                    static_cast<int64_t>(kTtsPcmStallTimeoutUs)) {
                break;
            }
        } while (true);
        const int64_t ended_us = esp_timer_get_time();
        const unsigned long elapsed_ms = static_cast<unsigned long>(
            (ended_us - started_us) / 1000);
        if (elapsed_ms >= 250 || read_len <= 0) {
            ESP_LOGW(TAG,
                     "TTS HTTP read: session=%lu, seq=%lu, request=%d, "
                     "read=%d, elapsed=%lums, retries=%lu, errno=%d",
                     static_cast<unsigned long>(session_id),
                     static_cast<unsigned long>(read_sequence),
                     event->buffer_len, read_len, elapsed_ms,
                     static_cast<unsigned long>(eagain_retries),
                     read_errno);
        }
        return read_len;
    }
    if (event->event_id != HTTP_STREAM_POST_REQUEST) {
        return ESP_GMF_ERR_OK;
    }

    PlaybackEvent playback_event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_ || http_request_ready_emitted_ ||
            current_request_.channel != PlaybackChannel::kTts) {
            return ESP_GMF_ERR_OK;
        }
        tts_http_client_ = event->http_client;
        http_request_ready_emitted_ = true;
        playback_event.session_id = current_request_.session_id;
        playback_event.channel = current_request_.channel;
        playback_event.state = PlaybackState::kStreamReady;
    }
    ESP_LOGI(TAG, "TTS HTTP 音频请求已发出: session=%lu, elapsed=%lums",
             static_cast<unsigned long>(playback_event.session_id),
             static_cast<unsigned long>(esp_log_timestamp() - playback_started_ms_));
    Emit(playback_event);
    return ESP_GMF_ERR_OK;
}

int GmfPlayerEngine::EventCallbackAdapter(esp_asp_event_pkt_t* event, void* ctx) {
    return static_cast<GmfPlayerEngine*>(ctx)->HandleEvent(event);
}

int GmfPlayerEngine::HandleOutput(uint8_t* data, int data_size) {
    if (data_size <= 0 || audio_service_ == nullptr) {
        return ESP_FAIL;
    }
    uint32_t session_id = 0;
    uint32_t chunk_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_id = current_request_.session_id;
        chunk_index = ++output_chunks_;
        if (playing_ && current_request_.channel == PlaybackChannel::kTts &&
            current_request_.session_id == tts_timeout_session_id_ &&
            !tts_timeout_stop_pending_) {
            const int64_t now_us = esp_timer_get_time();
            last_tts_pcm_us_ = now_us;
        }
    }

    if (chunk_index == 1) {
        ESP_LOGI(TAG, "首个 PCM 输出: session=%lu, elapsed=%lums, bytes=%d",
                 static_cast<unsigned long>(session_id),
                 static_cast<unsigned long>(esp_log_timestamp() - playback_started_ms_),
                 data_size);
    }
    /* 播放器固定输出 CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE，
       由 AudioService 在入播放队列前重采样到 codec 实际输出采样率 */
    return audio_service_->PushPcmToPlaybackQueue(
               data, static_cast<size_t>(data_size),
               CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE, true)
               ? ESP_OK
               : ESP_FAIL;
}

int GmfPlayerEngine::HandleEvent(esp_asp_event_pkt_t* event) {
    if (event == nullptr || event->payload == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
        event->payload_size == sizeof(esp_asp_music_info_t)) {
        esp_asp_music_info_t info = {};
        std::memcpy(&info, event->payload, sizeof(info));
        ESP_LOGI(TAG, "MP3 解码信息: elapsed=%lums, source=%d Hz/%u ch/%u bit, output=%d Hz/%d ch/%d bit",
                 static_cast<unsigned long>(esp_log_timestamp() - playback_started_ms_),
                 info.sample_rate, info.channels, info.bits,
                 CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE,
                 CONFIG_AUDIO_SIMPLE_PLAYER_CH_CVT_DEST,
                 CONFIG_AUDIO_SIMPLE_PLAYER_BIT_CVT_DEST_BITS);
        return ESP_OK;
    }
    if (event->type != ESP_ASP_EVENT_TYPE_STATE || event->payload_size != sizeof(esp_asp_state_t)) {
        return ESP_OK;
    }

    esp_asp_state_t state = ESP_ASP_STATE_NONE;
    std::memcpy(&state, event->payload, sizeof(state));

    PlaybackEvent playback_event;
    bool should_emit = true;
    bool terminal = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playback_event.session_id = current_request_.session_id;
        playback_event.channel = current_request_.channel;
        switch (state) {
            case ESP_ASP_STATE_RUNNING:
                playing_ = true;
                draining_ = false;
                playback_event.state = PlaybackState::kPlaying;
                break;
            case ESP_ASP_STATE_PAUSED:
                playback_event.state = PlaybackState::kPaused;
                break;
            case ESP_ASP_STATE_STOPPED:
                playing_ = false;
                draining_ = false;
                command_pending_playback_ = false;
                playback_event.reason = stop_reason_ == PlaybackEndReason::kNone
                                            ? PlaybackEndReason::kCancelled
                                            : stop_reason_;
                playback_event.state = IsTtsPcmTimeout(playback_event.reason)
                                           ? PlaybackState::kFailed
                                           : PlaybackState::kStopped;
                terminal = true;
                break;
            case ESP_ASP_STATE_FINISHED:
                playing_ = false;
                draining_ = true;
                command_pending_playback_ = false;
                playback_event.state = PlaybackState::kCompleted;
                playback_event.reason = PlaybackEndReason::kNaturalEnd;
                terminal = true;
                break;
            case ESP_ASP_STATE_ERROR:
                playing_ = false;
                draining_ = false;
                command_pending_playback_ = false;
                playback_event.state = PlaybackState::kFailed;
                playback_event.reason = IsTtsPcmTimeout(stop_reason_)
                                            ? stop_reason_
                                            : PlaybackEndReason::kFailed;
                terminal = true;
                break;
            default:
                should_emit = false;
                break;
        }
        if (terminal) {
            tts_http_client_ = nullptr;
            tts_timeout_session_id_ = 0;
            tts_timeout_started_us_ = 0;
            last_tts_pcm_us_ = 0;
            tts_timeout_stop_pending_ = false;
        }
    }

    if (!should_emit) {
        return ESP_OK;
    }
    if (terminal) {
        DisarmTtsPcmTimeout();
    }
    if (state == ESP_ASP_STATE_ERROR) {
        NetworkDiagnostics::LogSnapshot("player_stream_error");
    }
    ESP_LOGI(TAG, "GMF 状态事件: state=%s, elapsed=%lums, pcm_chunks=%lu",
             esp_audio_simple_player_state_to_str(state),
             static_cast<unsigned long>(esp_log_timestamp() - playback_started_ms_),
             static_cast<unsigned long>(output_chunks_));
    if (state == ESP_ASP_STATE_FINISHED) {
        audio_service_->MarkPlayerPlaybackFinished();
        // GMF 完成表示解码已结束；还要等待 AudioOutputTask 写完最后一批 PCM。
        audio_service_->WaitForPlaybackComplete();
        std::lock_guard<std::mutex> lock(mutex_);
        draining_ = false;
    } else if (state == ESP_ASP_STATE_STOPPED || state == ESP_ASP_STATE_ERROR) {
        audio_service_->ClearPlaybackQueue();
    }

    ESP_LOGI(TAG, "播放状态: session=%lu, state=%s",
             static_cast<unsigned long>(playback_event.session_id),
             esp_audio_simple_player_state_to_str(state));
    Emit(playback_event);
    return ESP_OK;
}

void GmfPlayerEngine::Emit(const PlaybackEvent& event) {
    if (event_callback_) {
        event_callback_(event);
    }
}
