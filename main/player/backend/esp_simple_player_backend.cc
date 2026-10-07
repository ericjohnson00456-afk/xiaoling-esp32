#include "esp_simple_player_backend.h"

#include "resource/resource.h"
#include "xiaoling/conn/network_diagnostics.h"

#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <esp_audio_simple_player_advance.h>
#include <esp_gmf_io_embed_flash.h>
#include <esp_gmf_io_file.h>
#include <esp_gmf_io_http.h>
#include <esp_gmf_pipeline.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

#include <cstring>
#include <cerrno>
#include <array>
#include <limits>
#include <strings.h>
#include <sys/socket.h>
#include <utility>
#include <vector>

#define TAG "EspSimplePlayerBackend"

namespace {

// esp_http_client_read() 会一直读取，直到填满调用方要求的长度。
// 如果一次请求 8 KiB，实时 MP3 流就会被积攒成持续数秒的音频突发。
// 将单次网络读取控制在较小范围，同时保留足够的异步缓冲，吸收云端
// 短暂的生成停顿。
// 实时 TTS 响应可能只包含下一个 MP3 帧的一部分。云端按片段生成语音，
// 每次返回的数据可能不足一个完整 MP3 帧，因此要尽快把已有字节交给解码器，
// 不要让 esp_http_client_read() 等待更大的数据块。
// 当前 48 kbps/16 kHz 流的一个 MP3 帧约为 216 字节。每次读取 1 KiB，
// 可以及时消耗服务器突发数据并保持较小的 lwIP 接收窗口流动；下面的
// 50 ms 正文超时也能在数据不足时及时返回部分内容。
constexpr size_t kStreamingHttpReadSize = 1024;
constexpr size_t kStreamingHttpBufferSize = 32 * 1024;
// Local files must be read ahead independently of MP3 decoding. This is
// especially important when an SD card shares its SPI bus with the LCD: a
// synchronous fread/decode chain cannot sustain high-bitrate tracks while
// display flushes are active.
constexpr size_t kFileReadSize = 4 * 1024;
constexpr size_t kFileBufferSize = 32 * 1024;
constexpr size_t kFileInputStackSize = 4 * 1024;
constexpr uint32_t kFileDecodeYieldChunkInterval = 8;
constexpr char kBufferedFileIoName[] = "io_file_buffered";
// 建链阶段由 HTTP 客户端先在 10 秒返回，外层 12 秒看门狗负责兜底；请求
// 真正发出后重新给首帧 8 秒，播放过程中仍以 5 秒实际无声作为断流阈值。
constexpr int kTtsHttpConnectTimeoutMs = 10 * 1000;
constexpr uint64_t kTtsConnectWatchdogUs = 12ULL * 1000ULL * 1000ULL;
constexpr uint64_t kTtsFirstPcmTimeoutUs = 8ULL * 1000ULL * 1000ULL;
constexpr uint64_t kTtsPcmStallTimeoutUs = 5ULL * 1000ULL * 1000ULL;
// esp_http_client_read() 默认会等待完整的请求长度。较短的正文读取超时可以让
// 稀疏的实时 TTS 数据一到达就返回部分正文，同时保留 GMF 对请求头和连接阶段
// 的默认超时设置。
constexpr int kStreamingHttpBodyReadTimeoutMs = 50;
constexpr size_t kPlayerCommandStackSize = 4096;
// AudioPlayer 的控制、网络拉流和解码任务统一使用优先级 8。
constexpr UBaseType_t kPlayerCommandPriority = 8;
constexpr UBaseType_t kPlayerInputPriority = 8;
constexpr UBaseType_t kPlayerDecodePriority = 8;
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

esp_gmf_err_t ScoreBufferedFileIo(esp_gmf_io_handle_t, const char* url,
                                  int* score) {
    if (url == nullptr || score == nullptr) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    *score = ESP_GMF_IO_SCORE_NONE;
    if (url[0] == '/' || strncasecmp(url, "file://", 7) == 0) {
        // Beat the synchronous file IO's STANDARD score so the pool clones
        // this already-BLOCK prototype while constructing the pipeline.
        *score = ESP_GMF_IO_SCORE_PERFECT;
    }
    return ESP_GMF_ERR_OK;
}

}  // namespace

// 配置 esp_audio_simple_player 创建出的输入 IO。
// HTTP 和文件输入统一调整任务调度；ROMFS 输入绑定提示音资源表。
int EspSimplePlayerBackend::ConfigureInputIo(esp_asp_handle_t handle, void* ctx) {
    auto* engine = static_cast<EspSimplePlayerBackend*>(ctx);
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
    const bool is_file = input_tag != nullptr &&
                         strcasecmp(input_tag, kBufferedFileIoName) == 0;

    if (is_file) {
        ESP_LOGI(TAG,
                 "文件输入任务已配置: read=%u, buffer=%u bytes, "
                 "priority=%u, core=%d",
                 static_cast<unsigned>(kFileReadSize),
                 static_cast<unsigned>(kFileBufferSize),
                 static_cast<unsigned>(kPlayerInputPriority),
                 kPlayerCommandCore);
        return ESP_GMF_ERR_OK;
    }

    if (is_http) {
        // 上一条流式 TTS 可能通过 IO abort 强制退出；新请求运行前必须清除
        // abort 标志并重新释放 HTTP 输入任务。
        esp_gmf_io_clear_abort(input);
        // 在 esp_gmf_pipeline_run() 打开 IO 并创建任务、数据总线之前配置克隆出的
        // Pipeline IO。这里只覆盖播放实例的 HTTP 参数，不修改 managed_components。
        auto* io = static_cast<esp_gmf_io_t*>(input);
        // 网络拉流任务需要先于解码和输出任务准备数据，同时避免 TTS 网络工作
        // 占用运行 AFE 的 0 号核心。
        io->io_cfg.thread.prio = kPlayerInputPriority;
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
                 static_cast<unsigned>(kPlayerInputPriority),
                 kPlayerCommandCore);
        // 保留组件原有的 acquire_read 实现。上面的回调只观察请求生命周期，
        // 这里保存的后端上下文与播放器句柄具有相同的生命周期。
        return ESP_GMF_ERR_OK;
    }

    if (input_tag == nullptr || strcasecmp(input_tag, "io_embed_flash") != 0) {
        return ESP_GMF_ERR_OK;
    }
    std::array<embed_item_info_t, Resource::kToneCount> items{};
    for (uint16_t id = 0; id < Resource::kToneCount; ++id) {
        ResourceData resource;
        if (!Resource::GetTone(id, resource)) {
            continue;
        }
        if (resource.size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            ESP_LOGE(TAG, "离线音频资源过大: id=%u, size=%u",
                     static_cast<unsigned>(id), static_cast<unsigned>(resource.size));
            return ESP_GMF_ERR_INVALID_ARG;
        }
        items[id] = {
            .address = resource.address,
            .size = static_cast<int>(resource.size),
        };
    }
    return esp_gmf_io_embed_flash_set_context(
        input, items.data(), static_cast<int>(items.size()));
}

EspSimplePlayerBackend::~EspSimplePlayerBackend() {
    // 先停止控制任务和超时定时器，再销毁播放器句柄，确保回调不会访问已释放对象。
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

bool EspSimplePlayerBackend::Initialize(AudioOutput* audio_output, EventCallback callback) {
    if (initialized_) {
        return true;
    }
    if (audio_output == nullptr) {
        return false;
    }

    /* 不要在 115200 波特率下打印每一次 HTTP 读取和解码探测日志。
       启动 TTS 时过多串口日志会阻塞流式播放路径；状态和错误日志仍使用
       组件默认级别输出。 */
    esp_log_level_set("ESP_GMF_HTTP", ESP_LOG_WARN);
    esp_log_level_set("ESP_GMF_ASMP_DEC", ESP_LOG_WARN);
    esp_log_level_set("ESP_GMF_TASK", ESP_LOG_WARN);
    esp_log_level_set("ESP_GMF_PORT", ESP_LOG_WARN);
    esp_log_level_set("ESP_GMF_BLOCK", ESP_LOG_WARN);
    esp_log_level_set("ESP_GMF_EMBED_FLASH", ESP_LOG_WARN);

    // Simple Player 内部包含解码任务；输出回调把解码后的 PCM 交给 AudioOutput。
    esp_asp_cfg_t config = {};
    config.out.cb = OutputCallback;
    config.out.user_ctx = this;
    // 解码任务与网络输入任务一样运行在次级核心。
    config.task_prio = kPlayerDecodePriority;
    config.task_stack = 8 * 1024;
    config.task_core = 1;
    // 解码任务使用 8 KiB 栈，并放在 PSRAM 中，以便在多个 TLS 流并发时
    // 为 Wi-Fi/lwIP 和 DMA 保留紧张的片上 SRAM。
    config.task_stack_in_ext = true;
    config.prev = ConfigureInputIo;
    config.prev_ctx = this;

    const auto result = esp_audio_simple_player_new(&config, &handle_);
    if (result != ESP_GMF_ERR_OK || handle_ == nullptr) {
        ESP_LOGE(TAG, "创建 GMF 播放器失败: 0x%x", result);
        handle_ = nullptr;
        return false;
    }

    // The file IO must be asynchronous before GMF clones it into a pipeline.
    // Configuring the cloned default BYTE IO in ConfigureInputIo() is too late:
    // its BYTE port has already allocated a payload, which is then lost when
    // the asynchronous data bus supplies another buffer.
    file_io_cfg_t file_cfg = FILE_IO_CFG_DEFAULT();
    file_cfg.dir = ESP_GMF_IO_DIR_READER;
    file_cfg.name = kBufferedFileIoName;
    file_cfg.io_cfg.thread.stack = kFileInputStackSize;
    file_cfg.io_cfg.thread.prio = kPlayerInputPriority;
    file_cfg.io_cfg.thread.core = kPlayerCommandCore;
    file_cfg.io_cfg.thread.stack_in_ext = true;
    file_cfg.io_cfg.buffer_cfg.io_size = kFileReadSize;
    file_cfg.io_cfg.buffer_cfg.buffer_size = kFileBufferSize;

    esp_gmf_io_handle_t file_io = nullptr;
    auto file_result = esp_gmf_io_file_init(&file_cfg, &file_io);
    if (file_result == ESP_GMF_ERR_OK && file_io != nullptr) {
        static_cast<esp_gmf_io_t*>(file_io)->get_score = ScoreBufferedFileIo;
        file_result = esp_audio_simple_player_register_io(handle_, file_io);
    }
    if (file_result != ESP_GMF_ERR_OK || file_io == nullptr) {
        ESP_LOGE(TAG, "注册异步文件输入失败: 0x%x", file_result);
        if (file_io != nullptr) {
            esp_gmf_obj_delete(file_io);
        }
        esp_audio_simple_player_destroy(handle_);
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

    audio_output_ = audio_output;
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
            static_cast<EspSimplePlayerBackend*>(arg)->HandleTtsPcmTimeout();
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

bool EspSimplePlayerBackend::Play(const PlaybackRequest& request) {
    if (!initialized_ || handle_ == nullptr ||
        (request.source_type != PlaybackSourceType::kUrl &&
         request.source_type != PlaybackSourceType::kRomfs &&
         request.source_type != PlaybackSourceType::kFile)) {
        return false;
    }

    // 所有播放器操作都进入同一个控制任务，避免多个线程同时调用 GMF。
    command_pending_playback_ = true;
    return Enqueue(Command{
        .type = CommandType::kPlay,
        .request = request,
    });
}

bool EspSimplePlayerBackend::DoPlay(const PlaybackRequest& request) {
    // 以下逻辑只在播放器控制任务中执行：清理旧会话、启动新流并记录会话状态。
    if (IsPlaying()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (playing_ && current_request_.channel == PlaybackChannel::kTts) {
                tts_stop_requested_ = true;
            }
        }
        if (!DoStop(PlaybackEndReason::kPreempted)) {
            ESP_LOGE(TAG, "无法停止当前播放会话");
            command_pending_playback_ = false;
            return false;
        }
    }
    DisarmTtsPcmTimeout();

    // 新媒体开始前清掉旧下行 Opus 和残留 PCM，物理 I2S 仍由 AudioOutputTask 独占。
    const bool local_file = request.source_type == PlaybackSourceType::kFile;
    audio_output_->BeginPlayback(AudioPlaybackConfig{
        .session_id = request.session_id,
        .initial_prebuffer_bytes = local_file
                                       ? kAudioLocalFilePrebufferBytes
                                       : (request.channel == PlaybackChannel::kTts
                                              ? kAudioTtsInitialPrebufferBytes
                                              : kAudioPlayerInitialPrebufferBytes),
        .recovery_prebuffer_bytes = local_file
                                        ? kAudioLocalFilePrebufferBytes
                                        : kAudioPlayerInitialPrebufferBytes,
        .max_buffered_pcm_bytes = local_file
                                      ? kAudioLocalFileMaxBufferedBytes
                                      : 0,
        .producer_resume_pcm_bytes = local_file
                                         ? kAudioLocalFileProducerResumeBytes
                                         : 0,
    });
    playback_started_ms_ = esp_log_timestamp();
    if (request.source_type == PlaybackSourceType::kFile) {
        ESP_LOGI(TAG, "开始文件播放: session=%lu, channel=%d, path=%s",
                 static_cast<unsigned long>(request.session_id),
                 static_cast<int>(request.channel), request.source.c_str());
    } else {
        ESP_LOGI(TAG, "开始流式播放: session=%lu, channel=%d, host=%s, url_bytes=%u",
                 static_cast<unsigned long>(request.session_id),
                 static_cast<int>(request.channel),
                 UrlHostForLog(request.source).c_str(),
                 static_cast<unsigned>(request.source.size()));
    }
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
        tts_stop_requested_ = false;
    }

    output_chunks_ = 0;
    const auto result = esp_audio_simple_player_run(handle_, request.source.c_str(), nullptr);
    if (result == ESP_GMF_ERR_OK) {
        if (request.channel == PlaybackChannel::kTts) {
            ArmTtsPcmTimeout(kTtsConnectWatchdogUs);
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
        tts_stop_requested_ = false;
    }
    audio_output_->ClearActivePlaybackQueue();
    command_pending_playback_ = false;
    Emit(PlaybackEvent{
        .session_id = request.session_id,
        .channel = request.channel,
        .state = PlaybackState::kFailed,
        .reason = PlaybackEndReason::kFailed,
    });
    return false;
}

bool EspSimplePlayerBackend::Pause() {
    if (!command_task_running_ || command_task_handle_ == nullptr) {
        return false;
    }

    // Pause 需要等待 GMF 真正进入 PAUSED，调用方才能安全地覆盖提示音或 TTS。
    std::unique_lock<std::mutex> lock(command_mutex_);
    const uint32_t request_id = ++next_pause_request_id_;
    command_queue_.push_back(Command{
        .type = CommandType::kPause,
        .completion_id = request_id,
    });
    xTaskNotifyGive(command_task_handle_);
    command_cv_.wait(lock, [this, request_id]() {
        return completed_pause_request_id_ >= request_id ||
               !command_task_running_;
    });
    return completed_pause_request_id_ >= request_id && last_pause_result_;
}

bool EspSimplePlayerBackend::DoPause() {
    bool is_tts = false;
    uint32_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || handle_ == nullptr || !playing_) {
            return false;
        }
        is_tts = current_request_.channel == PlaybackChannel::kTts;
        session_id = current_request_.session_id;
    }

    // GMF 的 pause 会同步等待 PAUSED 状态回调，而回调也需要 mutex_。
    // 因此不能在持锁期间调用 pause，否则两边会互相等待直到 GMF 超时。
    const bool paused =
        esp_audio_simple_player_pause(handle_) == ESP_GMF_ERR_OK;
    if (paused && is_tts) {
        DisarmTtsPcmTimeout();
    }
    if (paused) {
        ESP_LOGI(TAG, "播放已暂停: session=%lu",
                 static_cast<unsigned long>(session_id));
    }
    return paused;
}

bool EspSimplePlayerBackend::Resume() {
    // Resume 不需要同步等待，实际调用由控制任务串行执行。
    return Enqueue(Command{.type = CommandType::kResume});
}

bool EspSimplePlayerBackend::DoResume() {
    // 恢复解码器后，TTS 重新开始计算首帧或连续 PCM 的超时。
    bool is_tts = false;
    bool request_ready = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || handle_ == nullptr || !playing_) {
            return false;
        }
        is_tts = current_request_.channel == PlaybackChannel::kTts;
        request_ready = http_request_ready_emitted_;
    }

    // resume 同样会同步触发 RUNNING 状态回调，必须在 mutex_ 外调用。
    const bool resumed =
        esp_audio_simple_player_resume(handle_) == ESP_GMF_ERR_OK;
    if (resumed && is_tts) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const int64_t now_us = esp_timer_get_time();
            if (output_chunks_.load() == 0) {
                tts_timeout_started_us_ = now_us;
            } else {
                last_tts_pcm_us_ = now_us;
            }
            tts_timeout_stop_pending_ = false;
        }
        ArmTtsPcmTimeout(output_chunks_.load() == 0
                             ? (request_ready ? kTtsFirstPcmTimeoutUs
                                              : kTtsConnectWatchdogUs)
                             : kTtsPcmStallTimeoutUs);
    }
    return resumed;
}

bool EspSimplePlayerBackend::Stop(PlaybackEndReason reason) {
    // Stop 同样异步投递，状态终态由 GMF 回调统一确认。
    command_pending_playback_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playing_ && current_request_.channel == PlaybackChannel::kTts) {
            // 流式 TTS 可能正在等云端开始合成。先让 50 ms 正文读取循环退出，
            // 控制任务才能及时完成 stop，避免后续提示音被堵在队列中。
            tts_stop_requested_ = true;
        }
    }
    return Enqueue(Command{
        .type = CommandType::kStop,
        .reason = reason,
    });
}

bool EspSimplePlayerBackend::DoStop(PlaybackEndReason reason) {
    // 只有控制任务能调用底层 stop，避免停止和播放命令交叉进入 GMF。
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
            if (current_request_.channel == PlaybackChannel::kTts) {
                tts_stop_requested_ = true;
            }
        }
    }
    if (drain_only) {
        audio_output_->ClearActivePlaybackQueue();
        return true;
    }

    if (tts_stop_requested_.load()) {
        InterruptTtsHttpSocket();
    }

    const auto result = esp_audio_simple_player_stop(handle_);
    if (result != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "停止播放失败: 0x%x", result);
        return false;
    }
    return true;
}

bool EspSimplePlayerBackend::IsPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_ || draining_ || command_pending_playback_.load();
}

bool EspSimplePlayerBackend::Enqueue(Command command) {
    if (!command_task_running_ || command_task_handle_ == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        // 多个打断来源可能同时请求停止。同一时刻只保留一个待处理的停止命令，
        // 避免反复进入 GMF。
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

void EspSimplePlayerBackend::CommandTaskEntry(void* arg) {
    auto* engine = static_cast<EspSimplePlayerBackend*>(arg);
    engine->CommandLoop();
    const bool external_stack = engine->command_task_stack_external_;
    engine->command_task_finished_ = true;
    engine->command_cv_.notify_all();
    if (external_stack) {
        vTaskDeleteWithCaps(nullptr);
    } else {
        vTaskDelete(nullptr);
    }
}

void EspSimplePlayerBackend::CommandLoop() {
    // 控制任务负责串行处理播放、暂停、恢复和停止命令。
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
                case CommandType::kPause: {
                    const bool result = DoPause();
                    if (command.completion_id != 0) {
                        {
                            std::lock_guard<std::mutex> lock(command_mutex_);
                            completed_pause_request_id_ = command.completion_id;
                            last_pause_result_ = result;
                        }
                        command_cv_.notify_all();
                    }
                    break;
                }
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

void EspSimplePlayerBackend::StopCommandTask() {
    // 析构时发送关闭命令，并等待控制任务完成，避免后台任务继续使用成员变量。
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

void EspSimplePlayerBackend::ArmTtsPcmTimeout(uint64_t timeout_us) {
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

void EspSimplePlayerBackend::DisarmTtsPcmTimeout() {
    std::lock_guard<std::mutex> lock(tts_timer_mutex_);
    if (tts_pcm_timer_ != nullptr) {
        esp_timer_stop(tts_pcm_timer_);
    }
}

void EspSimplePlayerBackend::InterruptTtsHttpSocket() {
    esp_gmf_pipeline_handle_t pipeline = nullptr;
    esp_gmf_io_handle_t input = nullptr;
    if (handle_ != nullptr &&
        esp_audio_simple_player_get_pipeline(handle_, &pipeline) == ESP_GMF_ERR_OK &&
        pipeline != nullptr &&
        esp_gmf_pipeline_get_in(pipeline, &input) == ESP_GMF_ERR_OK &&
        input != nullptr) {
        const auto abort_result = esp_gmf_io_abort(input);
        ESP_LOGI(TAG, "TTS 停止时中止 GMF HTTP 输入: result=0x%x",
                 static_cast<unsigned>(abort_result));
    }

    void* http_client = tts_http_client_.exchange(nullptr);
    if (http_client == nullptr) {
        return;
    }
    const int fd = esp_http_client_get_socket(
        static_cast<esp_http_client_handle_t>(http_client));
    if (fd < 0) {
        return;
    }
    errno = 0;
    const int result = shutdown(fd, SHUT_RDWR);
    const int saved_errno = result == 0 ? 0 : errno;
    ESP_LOGI(TAG,
             "TTS 停止时中断 HTTP socket: fd=%d, result=%d, errno=%d(%s)",
             fd, result, saved_errno, std::strerror(saved_errno));
}

void EspSimplePlayerBackend::HandleTtsPcmTimeout() {
    // 分别限制 HTTP 建链、请求发出后的首帧以及播放中的实际无声时间。
    PlaybackEndReason timeout_reason = PlaybackEndReason::kNone;
    uint32_t session_id = 0;
    uint64_t remaining_us = 0;
    uint64_t timeout_interval_us = 0;
    const char* timeout_type = "unknown";
    void* http_client = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_ || current_request_.channel != PlaybackChannel::kTts ||
            current_request_.session_id != tts_timeout_session_id_ ||
            tts_timeout_stop_pending_) {
            return;
        }
        const bool received_pcm = output_chunks_.load() != 0;
        const bool request_ready = http_request_ready_emitted_;
        int64_t reference_us = received_pcm
                                   ? last_tts_pcm_us_
                                   : tts_timeout_started_us_;
        if (received_pcm) {
            /* 播放开始后，只要 PCM 缓冲仍在排空，上游合成暂时停顿就不会造成
             * 实际无声。因此使用扬声器最近一次完成输出的时间作为截止点，
             * 让五秒超时真正测量扬声器的无声时长。 */
            const uint32_t last_output_ms =
                audio_output_->GetOutputLastActivityMs();
            if (last_output_ms != 0) {
                reference_us = static_cast<int64_t>(last_output_ms) * 1000;
            }
        }
        if (reference_us <= 0) {
            return;
        }
        /* 必须在获取 mutex_ 后读取当前时间。定时器等待锁期间，PCM 回调可能
         * 已经刷新参考时间；如果使用加锁前的时间戳，减法可能变成负数，
         * 转成无符号数后会被误认为是一个很大的超时时间。 */
        const int64_t now_us = esp_timer_get_time();
        timeout_interval_us = received_pcm
                                  ? kTtsPcmStallTimeoutUs
                                  : (request_ready ? kTtsFirstPcmTimeoutUs
                                                   : kTtsConnectWatchdogUs);
        timeout_type = received_pcm
                           ? "pcm_stall"
                           : (request_ready ? "first_pcm" : "connect");
        const uint64_t elapsed_us = now_us >= reference_us
                                        ? static_cast<uint64_t>(now_us - reference_us)
                                        : 0;
        if (elapsed_us < timeout_interval_us) {
            remaining_us = timeout_interval_us - elapsed_us;
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
        timeout_interval_us / 1000ULL);
    ESP_LOGE(TAG, "TTS PCM 超时: session=%lu, type=%s, timeout=%lums",
             static_cast<unsigned long>(session_id),
             timeout_type,
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
        ArmTtsPcmTimeout(timeout_interval_us);
    }
}

int EspSimplePlayerBackend::OutputCallback(uint8_t* data, int data_size, void* ctx) {
    return static_cast<EspSimplePlayerBackend*>(ctx)->HandleOutput(data, data_size);
}

int EspSimplePlayerBackend::HttpIoEventCallback(http_stream_event_msg_t* event) {
    if (event == nullptr || event->user_data == nullptr) {
        return ESP_GMF_ERR_OK;
    }
    return static_cast<EspSimplePlayerBackend*>(event->user_data)->HandleHttpIoEvent(event);
}

int EspSimplePlayerBackend::HandleHttpIoEvent(http_stream_event_msg_t* event) {
    if (event == nullptr) {
        return ESP_GMF_ERR_OK;
    }

    // PRE_REQUEST 发生在 connect 之前；先缩短组件默认的 30 秒连接超时，再保存
    // 句柄供外层看门狗在已经取得 socket 后主动中断。
    if (event->event_id == HTTP_STREAM_PRE_REQUEST) {
        const auto client =
            static_cast<esp_http_client_handle_t>(event->http_client);
        const esp_err_t timeout_result =
            esp_http_client_set_timeout_ms(client, kTtsHttpConnectTimeoutMs);
        if (timeout_result != ESP_OK) {
            ESP_LOGW(TAG, "TTS HTTP 建链超时设置失败: %s",
                     esp_err_to_name(timeout_result));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (playing_ && current_request_.channel == PlaybackChannel::kTts) {
            // 保存 HTTP 句柄，让五秒超时时可以中断正在 esp_http_client 内部
            // 等待数据的读取操作。
            tts_http_client_ = event->http_client;
        }
        return ESP_GMF_ERR_OK;
    }
    // ON_RESPONSE 接管 TTS 正文读取，让稀疏的实时音频数据尽快进入解码器。
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

        // 返回正数表示回调已经填充了输出缓冲区。
        // 对正文短超时继续重试，直到收到数据、到达 EOF、请求被取消，
        // 或者达到统一的五秒截止时间。
        const auto client = static_cast<esp_http_client_handle_t>(event->http_client);
        const int64_t started_us = esp_timer_get_time();

        if (tts_stop_requested_.load()) {
            return 0;
        }

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

            if (tts_stop_requested_.load()) {
                // 返回 EOF 让 GMF 立即退出当前 HTTP 输入；真正的停止原因仍由
                // 控制任务中的 stop 命令记录并发布。
                read_len = 0;
                break;
            }

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

    // POST_REQUEST 表示 HTTP 请求已经真正发出，对外发布 StreamReady。
    PlaybackEvent playback_event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_ || http_request_ready_emitted_ ||
            current_request_.channel != PlaybackChannel::kTts) {
            return ESP_GMF_ERR_OK;
        }
        tts_http_client_ = event->http_client;
        http_request_ready_emitted_ = true;
        tts_timeout_started_us_ = esp_timer_get_time();
        playback_event.session_id = current_request_.session_id;
        playback_event.channel = current_request_.channel;
        playback_event.state = PlaybackState::kStreamReady;
    }
    ESP_LOGI(TAG, "TTS HTTP 音频请求已发出: session=%lu, elapsed=%lums",
             static_cast<unsigned long>(playback_event.session_id),
             static_cast<unsigned long>(esp_log_timestamp() - playback_started_ms_));
    ArmTtsPcmTimeout(kTtsFirstPcmTimeoutUs);
    Emit(playback_event);
    return ESP_GMF_ERR_OK;
}

int EspSimplePlayerBackend::EventCallbackAdapter(esp_asp_event_pkt_t* event, void* ctx) {
    return static_cast<EspSimplePlayerBackend*>(ctx)->HandleEvent(event);
}

int EspSimplePlayerBackend::HandleOutput(uint8_t* data, int data_size) {
    if (data_size <= 0 || audio_output_ == nullptr) {
        return ESP_FAIL;
    }
    // 解码器每次输出 PCM 都经过这里，再进入 AudioOutput 的播放队列。
    AudioOutputEvent output;
    uint32_t chunk_index = 0;
    bool is_file = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output.session_id = current_request_.session_id;
        chunk_index = ++output_chunks_;
        is_file = current_request_.source_type == PlaybackSourceType::kFile;
        if (playing_ && current_request_.channel == PlaybackChannel::kTts &&
            current_request_.session_id == tts_timeout_session_id_ &&
            !tts_timeout_stop_pending_) {
            const int64_t now_us = esp_timer_get_time();
            last_tts_pcm_us_ = now_us;
        }
    }

    if (chunk_index == 1) {
        ESP_LOGI(TAG, "首个 PCM 输出: session=%lu, elapsed=%lums, bytes=%d",
                 static_cast<unsigned long>(output.session_id),
                 static_cast<unsigned long>(esp_log_timestamp() - playback_started_ms_),
                 data_size);
    }
    /* 播放器固定输出 CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE，
       AudioService 会在放入播放队列前将其重采样到 codec 的实际输出采样率。 */
    const bool queued = audio_output_->PushPcm(
        data, static_cast<size_t>(data_size),
        CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE, output, true);
    // Buffered file IO can keep every GMF stage continuously runnable. Give
    // CPU1's Idle task a bounded scheduling opportunity without throttling
    // HTTP TTS, whose reads already block while waiting for network data.
    if (queued && is_file &&
        chunk_index % kFileDecodeYieldChunkInterval == 0) {
        vTaskDelay(1);
    }
    return queued ? ESP_OK : ESP_FAIL;
}

int EspSimplePlayerBackend::HandleEvent(esp_asp_event_pkt_t* event) {
    if (event == nullptr || event->payload == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // 先处理采样率、声道数等媒体信息，再处理播放器状态事件。
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

    // 将 ESP Simple Player 的状态转换为项目统一的 PlaybackEvent。
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
            tts_stop_requested_ = false;
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
        audio_output_->MarkPlaybackFinished(AudioOutputEvent{
            .session_id = playback_event.session_id,
        });
        // GMF 完成只表示解码结束，还要等待 AudioOutputTask 写完最后一批 PCM。
        audio_output_->WaitForPlaybackComplete();
        std::lock_guard<std::mutex> lock(mutex_);
        draining_ = false;
    } else if (state == ESP_ASP_STATE_STOPPED || state == ESP_ASP_STATE_ERROR) {
        audio_output_->ClearActivePlaybackQueue();
    }

    ESP_LOGI(TAG, "播放状态: session=%lu, state=%s",
             static_cast<unsigned long>(playback_event.session_id),
             esp_audio_simple_player_state_to_str(state));
    Emit(playback_event);
    return ESP_OK;
}

void EspSimplePlayerBackend::Emit(const PlaybackEvent& event) {
    // 回调只负责把事件交给 AudioPlayer；具体业务处理在 app_event 任务中完成。
    if (event_callback_) {
        event_callback_(event);
    }
}
