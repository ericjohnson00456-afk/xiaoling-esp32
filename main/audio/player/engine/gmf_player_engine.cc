#include "gmf_player_engine.h"

#include "audio_service.h"
#include "audio/tone/tone_model.h"

#include <esp_err.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <esp_audio_simple_player_advance.h>
#include <esp_gmf_io_embed_flash.h>
#include <esp_gmf_pipeline.h>

#include <cstring>
#include <strings.h>
#include <utility>

#define TAG "GmfPlayerEngine"

namespace {

int ConfigureRomfsToneIo(esp_asp_handle_t handle, void*) {
    auto& tones = ToneModel::GetInstance();
    if (!tones.initialized() || tones.EmbedItemCount() == 0) {
        return ESP_GMF_ERR_OK;
    }

    esp_gmf_pipeline_handle_t pipeline = nullptr;
    if (esp_audio_simple_player_get_pipeline(handle, &pipeline) != ESP_GMF_ERR_OK || pipeline == nullptr) {
        return ESP_GMF_ERR_FAIL;
    }
    esp_gmf_io_handle_t input = nullptr;
    if (esp_gmf_pipeline_get_in(pipeline, &input) != ESP_GMF_ERR_OK || input == nullptr) {
        return ESP_GMF_ERR_FAIL;
    }
    if (strcasecmp(OBJ_GET_TAG(input), "io_embed_flash") != 0) {
        return ESP_GMF_ERR_OK;
    }
    return esp_gmf_io_embed_flash_set_context(input, tones.EmbedItems(), tones.EmbedItemCount());
}

std::string UrlHostForLog(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return "invalid";
    }
    const auto host_start = scheme_end + 3;
    const auto host_end = url.find_first_of("/?#", host_start);
    return url.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
}

}  // namespace

GmfPlayerEngine::~GmfPlayerEngine() {
    if (handle_ == nullptr) {
        return;
    }
    if (IsPlaying()) {
        Stop(PlaybackEndReason::kCancelled);
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

    esp_asp_cfg_t config = {};
    config.out.cb = OutputCallback;
    config.out.user_ctx = this;
    config.task_prio = 5;
    config.task_stack = 6 * 1024;
    config.task_core = 1;
    config.task_stack_in_ext = false;
    config.prev = ConfigureRomfsToneIo;

    const auto result = esp_audio_simple_player_new(&config, &handle_);
    if (result != ESP_GMF_ERR_OK || handle_ == nullptr) {
        ESP_LOGE(TAG, "创建 GMF 播放器失败: 0x%x", result);
        handle_ = nullptr;
        return false;
    }
    if (esp_audio_simple_player_set_event(handle_, EventCallbackAdapter, this) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "注册 GMF 播放事件失败");
        esp_audio_simple_player_destroy(handle_);
        handle_ = nullptr;
        return false;
    }

    audio_service_ = audio_service;
    event_callback_ = std::move(callback);
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

    if (IsPlaying() && !Stop(PlaybackEndReason::kPreempted)) {
        ESP_LOGE(TAG, "无法停止当前播放会话");
        return false;
    }

    // 新媒体开始前清掉旧下行 Opus 和残留 PCM，物理 I2S 仍由 AudioOutputTask 独占。
    audio_service_->ResetDecoder();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_request_ = request;
        stop_reason_ = PlaybackEndReason::kNone;
        playing_ = true;
    }

    ESP_LOGI(TAG, "开始流式播放: session=%lu, channel=tts, host=%s, url_bytes=%u",
             static_cast<unsigned long>(request.session_id), UrlHostForLog(request.source).c_str(),
             static_cast<unsigned>(request.source.size()));
    const auto result = esp_audio_simple_player_run(handle_, request.source.c_str(), nullptr);
    if (result == ESP_GMF_ERR_OK) {
        return true;
    }

    ESP_LOGE(TAG, "启动流式播放失败: session=%lu, error=0x%x",
             static_cast<unsigned long>(request.session_id), result);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
    }
    audio_service_->ClearPlaybackQueue();
    Emit(PlaybackEvent{
        .session_id = request.session_id,
        .channel = request.channel,
        .state = PlaybackState::kFailed,
        .reason = PlaybackEndReason::kFailed,
    });
    return false;
}

bool GmfPlayerEngine::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || handle_ == nullptr || !playing_) {
        return false;
    }
    return esp_audio_simple_player_pause(handle_) == ESP_GMF_ERR_OK;
}

bool GmfPlayerEngine::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || handle_ == nullptr || !playing_) {
        return false;
    }
    return esp_audio_simple_player_resume(handle_) == ESP_GMF_ERR_OK;
}

bool GmfPlayerEngine::Stop(PlaybackEndReason reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_) {
            return true;
        }
        stop_reason_ = reason;
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
    return playing_;
}

int GmfPlayerEngine::OutputCallback(uint8_t* data, int data_size, void* ctx) {
    return static_cast<GmfPlayerEngine*>(ctx)->HandleOutput(data, data_size);
}

int GmfPlayerEngine::EventCallbackAdapter(esp_asp_event_pkt_t* event, void* ctx) {
    return static_cast<GmfPlayerEngine*>(ctx)->HandleEvent(event);
}

int GmfPlayerEngine::HandleOutput(uint8_t* data, int data_size) {
    if (data_size <= 0 || audio_service_ == nullptr) {
        return ESP_FAIL;
    }
    /* 播放器固定输出 CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE，
       由 AudioService 在入播放队列前重采样到 codec 实际输出采样率 */
    return audio_service_->PushPcmToPlaybackQueue(data, static_cast<size_t>(data_size),
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
        ESP_LOGI(TAG, "MP3 解码信息: source=%d Hz/%u ch/%u bit, output=%d Hz/%d ch/%d bit",
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playback_event.session_id = current_request_.session_id;
        playback_event.channel = current_request_.channel;
        switch (state) {
            case ESP_ASP_STATE_RUNNING:
                playing_ = true;
                playback_event.state = PlaybackState::kPlaying;
                break;
            case ESP_ASP_STATE_PAUSED:
                playback_event.state = PlaybackState::kPaused;
                break;
            case ESP_ASP_STATE_STOPPED:
                playing_ = false;
                playback_event.state = PlaybackState::kStopped;
                playback_event.reason = stop_reason_ == PlaybackEndReason::kNone
                                            ? PlaybackEndReason::kCancelled
                                            : stop_reason_;
                break;
            case ESP_ASP_STATE_FINISHED:
                playing_ = false;
                playback_event.state = PlaybackState::kCompleted;
                playback_event.reason = PlaybackEndReason::kNaturalEnd;
                break;
            case ESP_ASP_STATE_ERROR:
                playing_ = false;
                playback_event.state = PlaybackState::kFailed;
                playback_event.reason = PlaybackEndReason::kFailed;
                break;
            default:
                should_emit = false;
                break;
        }
    }

    if (!should_emit) {
        return ESP_OK;
    }
    if (state == ESP_ASP_STATE_FINISHED) {
        // GMF 完成表示解码已结束；还要等待 AudioOutputTask 写完最后一批 PCM。
        audio_service_->WaitForPlaybackComplete();
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
