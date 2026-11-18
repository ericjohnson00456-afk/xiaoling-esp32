#include "miniapp_hls.h"
#include "miniapp.h"
#include "miniapp_audio.h"
#include "miniapp_platform.h"
#include "application.h"
#include "alarm/alarm_service.h"
#include "boards/common/board.h"
#include "display/lvgl_display.h"
#include "system/time_sync.h"
#include "player/backend/esp_simple_player_backend.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
std::mutex audio_mutex, event_mutex, desired_mutex, speech_mutex;
TaskHandle_t audio_task = nullptr;
std::string desired_url;
bool desired_paused = false;
uint32_t desired_generation = 0;
std::atomic<const char *> public_audio_state{"idle"};
std::unique_ptr<EspSimplePlayerBackend> online, speech;
std::string audio_url;
uint32_t audio_session = 0;
PlaybackState audio_event = PlaybackState::kStopped;
bool user_paused = false, suspended = false, loading_cancelled = false;
std::atomic<uint32_t> speech_owner{0}, speech_session{0}, sound_session{0};
std::atomic<bool> speech_cancelled{false}, sound_active{false}, sound_interrupted{false};
std::atomic<PlaybackState> speech_event{PlaybackState::kStopped};

Application &App() {
    return Application::GetInstance();
}
AudioService &Output() {
    return App().GetAudioService();
}
uint32_t Session() {
    return App().GetAudioPlayer().AllocateSession();
}
bool SystemBusy() {
    const auto state = App().GetDeviceState();
    return miniapp_platform_alarm() || state == kDeviceStateConnecting ||
           state == kDeviceStateListening || state == kDeviceStateSpeaking ||
           App().GetAudioPlayer().IsWakeupPending() || App().GetAudioPlayer().IsPlaying();
}
bool OnlineBlocked() {
    return SystemBusy() || speech_owner.load() || sound_active.load();
}
void SetAudioEvent(uint32_t session, PlaybackState event) {
    std::lock_guard<std::mutex> lock(event_mutex);
    if (audio_session == session)
        audio_event = event == PlaybackState::kStopped ? PlaybackState::kFailed : event;
}
void StopOnline(bool keep_url) {
    uint32_t previous;
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        previous = audio_session;
        audio_session = 0;
        audio_event = PlaybackState::kStopped;
    }
    if (online)
        online->Stop(PlaybackEndReason::kCancelled);
    if (previous) {
        Output().ClearActivePlaybackQueue(previous);
        Output().ClearSuspendedPlaybackQueue(previous);
    }
    if (!keep_url)
        audio_url.clear();
    suspended = false;
}
void PauseOnline() {
    if (!online || suspended || audio_url.empty())
        return;
    PlaybackState event;
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        event = audio_event;
    }
    if (event == PlaybackState::kStreamReady) {
        StopOnline(true);
        loading_cancelled = true;
    } else if (event == PlaybackState::kPlaying && online->Pause()) {
        Output().SuspendPlayback();
        suspended = true;
    }
}
bool StartOnline() {
    if (!online) {
        auto next = std::make_unique<EspSimplePlayerBackend>();
        if (!next->Initialize(&Output(), [](const PlaybackEvent &event) {
                SetAudioEvent(event.session_id, event.state);
            }))
            return false;
        if (!MiniappRegisterHls(*next))
            return false;
        online = std::move(next);
    }
    const auto session = Session();
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        audio_session = session;
        audio_event = PlaybackState::kStreamReady;
    }
    loading_cancelled = false;
    PlaybackRequest request{.session_id = session,
                            .source_type = PlaybackSourceType::kUrl,
                            .channel = PlaybackChannel::kMusic,
                            .source = MiniappAudioUri(audio_url),
                            .priority = 20,
                            .resumable = true,
                            .pcm_buffer_limit = 48 * 1024};
    return online->Play(request);
}
} // namespace

extern "C" int miniapp_screen_width() {
    return Board::GetInstance().GetDisplay()->width();
}
extern "C" int miniapp_screen_height() {
    return Board::GetInstance().GetDisplay()->height();
}
extern "C" bool miniapp_has_screen() {
    return dynamic_cast<LvglDisplay *>(Board::GetInstance().GetDisplay()) &&
           miniapp_screen_width() > 0 && miniapp_screen_height() >= 16;
}
extern "C" bool miniapp_has_speaker() {
    auto *codec = Board::GetInstance().GetAudioCodec();
    return codec && codec->output_sample_rate() > 0;
}
extern "C" bool miniapp_has_led() {
    return Board::GetInstance().GetLed()->SupportsMiniapp();
}
extern "C" void miniapp_led_on() {
    Board::GetInstance().GetLed()->SetMiniapp(true, 0, 0);
}
extern "C" void miniapp_led_off() {
    Board::GetInstance().GetLed()->SetMiniapp(false, 0, 0);
}
extern "C" void miniapp_led_blink(uint32_t on, uint32_t off) {
    Board::GetInstance().GetLed()->SetMiniapp(true, on, off);
}
extern "C" bool miniapp_platform_alarm() {
    return AlarmService::GetInstance().IsRinging();
}
extern "C" bool miniapp_platform_busy() {
    return SystemBusy() || speech_owner.load();
}
extern "C" bool miniapp_time_valid() {
    return TimeSync::IsSystemTimeValid();
}
extern "C" int miniapp_get_time(timeval *tv) {
    return gettimeofday(tv, nullptr);
}
extern "C" int miniapp_localtime(tm *calendar) {
    time_t now = time(nullptr);
    return miniapp_time_valid() && localtime_r(&now, calendar) ? 0 : -1;
}
extern "C" int miniapp_utc_offset() {
    time_t now = time(nullptr);
    tm local{}, utc{};
    if (!miniapp_time_valid() || !localtime_r(&now, &local) || !gmtime_r(&now, &utc))
        return 0;
    utc.tm_isdst = local.tm_isdst;
    return static_cast<int>(difftime(mktime(&local), mktime(&utc)));
}
extern "C" void miniapp_platform_mode_changed(bool active) {
    App().Schedule([active]() {
        App().AbortSpeaking(kAbortReasonExit);
        if (active)
            App().GetIntentManager().Music().Exit();
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    });
}
extern "C" void miniapp_platform_interrupt() {
    sound_interrupted = true;
    const auto sound = sound_session.load();
    if (sound)
        Output().ClearActivePlaybackQueue(sound);
    std::lock_guard<std::mutex> lock(audio_mutex);
    PauseOnline();
    if (speech_owner.load())
        miniapp_platform_tts_cancel(speech_owner.load());
}
namespace {
const char *OnlineState() {
    if (audio_url.empty())
        return "idle";
    if (OnlineBlocked())
        return "interrupted";
    if (user_paused)
        return "paused";
    std::lock_guard<std::mutex> events(event_mutex);
    if (audio_event == PlaybackState::kFailed)
        return "error";
    if (loading_cancelled || audio_event == PlaybackState::kStreamReady)
        return "loading";
    if (audio_event == PlaybackState::kPlaying)
        return "playing";
    if (audio_event == PlaybackState::kPaused)
        return "paused";
    return "idle";
}
void AudioControl(void *) {
    uint32_t applied_generation = 0;
    for (;;) {
        std::string url;
        bool paused;
        uint32_t generation;
        {
            std::lock_guard<std::mutex> desired(desired_mutex);
            url = desired_url;
            paused = desired_paused;
            generation = desired_generation;
        }
        {
            std::lock_guard<std::mutex> control(audio_mutex);
            if (generation != applied_generation) {
                StopOnline(false);
                audio_url = std::move(url);
                loading_cancelled = !audio_url.empty();
                applied_generation = generation;
            }
            user_paused = paused;
            if (!audio_url.empty()) {
                if (OnlineBlocked() || user_paused)
                    PauseOnline();
                else if (suspended) {
                    uint32_t session;
                    {
                        std::lock_guard<std::mutex> events(event_mutex);
                        session = audio_session;
                    }
                    Output().ResumePlayback(AudioOutputEvent{.session_id = session});
                    if (online->Resume())
                        suspended = false;
                    else
                        SetAudioEvent(session, PlaybackState::kFailed);
                } else if (loading_cancelled && !StartOnline()) {
                    loading_cancelled = false;
                    std::lock_guard<std::mutex> events(event_mutex);
                    audio_event = PlaybackState::kFailed;
                }
            }
            const char *state = OnlineState();
            std::lock_guard<std::mutex> desired(desired_mutex);
            if (desired_generation == applied_generation)
                public_audio_state = state;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
}
bool EnsureAudioControl() {
    if (audio_task)
        return true;
    return xTaskCreate(AudioControl, "miniapp_audio", 4096, nullptr, 4, &audio_task) == pdPASS;
}
} // namespace
extern "C" bool miniapp_audio_play(const char *url) {
    std::lock_guard<std::mutex> desired(desired_mutex);
    if (!EnsureAudioControl())
        return false;
    try {
        desired_url = url;
    } catch (...) {
        return false;
    }
    desired_paused = false;
    ++desired_generation;
    public_audio_state = "loading";
    xTaskNotifyGive(audio_task);
    return true;
}
extern "C" void miniapp_audio_pause(bool paused) {
    std::lock_guard<std::mutex> desired(desired_mutex);
    desired_paused = paused;
    if (audio_task)
        xTaskNotifyGive(audio_task);
}
extern "C" void miniapp_audio_stop() {
    std::lock_guard<std::mutex> desired(desired_mutex);
    desired_url.clear();
    desired_paused = false;
    ++desired_generation;
    public_audio_state = "idle";
    if (audio_task)
        xTaskNotifyGive(audio_task);
}
extern "C" void miniapp_audio_release() {
    miniapp_audio_stop();
}
extern "C" void miniapp_audio_poll() {
    if (audio_task)
        xTaskNotifyGive(audio_task);
}
extern "C" const char *miniapp_audio_state() {
    return public_audio_state.load();
}
extern "C" bool miniapp_platform_tts_claim(uint32_t id) {
    uint32_t expected = 0;
    if (!speech_owner.compare_exchange_strong(expected, id))
        return false;
    speech_cancelled = false;
    return true;
}
extern "C" void miniapp_platform_tts_cancel(uint32_t id) {
    if (speech_owner.load() == id) {
        speech_cancelled = true;
        std::lock_guard<std::mutex> lock(speech_mutex);
        if (speech)
            speech->Stop(PlaybackEndReason::kCancelled);
    }
}
extern "C" void miniapp_platform_tts_release(uint32_t id) {
    speech_owner.compare_exchange_strong(id, 0);
}
extern "C" void miniapp_platform_tts(const char *text, uint32_t id, const char **result,
                                     const char **error) {
    struct Reply {
        std::mutex mutex;
        bool ready = false, ok = false;
        std::string url;
    };
    auto reply = std::make_shared<Reply>();
    const std::string input(text);
    App().Schedule([reply, input, id]() {
        if (speech_owner.load() != id || speech_cancelled ||
            !App().RequestTextTtsUrl(input, [reply](bool ok, std::string url) {
                std::lock_guard<std::mutex> lock(reply->mutex);
                reply->ok = ok;
                reply->url = std::move(url);
                reply->ready = true;
            })) {
            std::lock_guard<std::mutex> lock(reply->mutex);
            reply->ready = true;
        }
    });
    auto started = xTaskGetTickCount();
    bool submitted = false;
    while (speech_owner.load() == id && !speech_cancelled) {
        if (SystemBusy()) {
            *result = "interrupted";
            *error = nullptr;
            break;
        }
        if (xTaskGetTickCount() - started > pdMS_TO_TICKS(MINIAPP_TTS_TIMEOUT_MS)) {
            *error = "timeout";
            break;
        }
        if (!submitted) {
            std::string url;
            {
                std::lock_guard<std::mutex> lock(reply->mutex);
                if (reply->ready) {
                    if (!reply->ok)
                        break;
                    url = reply->url;
                }
            }
            if (!url.empty()) {
                {
                    std::lock_guard<std::mutex> lock(audio_mutex);
                    PauseOnline();
                }
                {
                    std::lock_guard<std::mutex> lock(speech_mutex);
                    if (!speech) {
                        auto next = std::make_unique<EspSimplePlayerBackend>();
                        if (!next->Initialize(&Output(), [](const PlaybackEvent &event) {
                                if (event.session_id == speech_session.load())
                                    speech_event = event.state;
                            }))
                            break;
                        speech = std::move(next);
                    }
                }
                speech_event = PlaybackState::kStreamReady;
                speech_session = Session();
                if (speech_cancelled || SystemBusy() || speech_owner.load() != id) {
                    *result = "interrupted";
                    *error = nullptr;
                    break;
                }
                if (!speech->Play(PlaybackRequest{.session_id = speech_session.load(),
                                                  .source_type = PlaybackSourceType::kUrl,
                                                  .channel = PlaybackChannel::kTts,
                                                  .source = url,
                                                  .priority = 50}))
                    break;
                submitted = true;
            }
        } else if (speech_event.load() == PlaybackState::kCompleted) {
            *result = "completed";
            *error = nullptr;
            break;
        } else if (speech_event.load() == PlaybackState::kFailed)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (speech_cancelled) {
        *result = "interrupted";
        *error = nullptr;
    }
    if (submitted && speech)
        speech->Stop(PlaybackEndReason::kCancelled);
    miniapp_platform_tts_release(id);
}
extern "C" void miniapp_platform_sound(const miniapp_score_t *score, bool loop, uint16_t hz,
                                       uint16_t ms, uint32_t generation,
                                       const volatile uint32_t *current,
                                       const volatile bool *paused) {
    if (!score && miniapp_platform_busy())
        return;
    sound_active = true;
    {
        std::lock_guard<std::mutex> lock(audio_mutex);
        PauseOnline();
    }
    miniapp_synth_t synth{};
    miniapp_synth_init(&synth);
    uint32_t position = 0, phase = 0;
    const uint32_t total = score ? score->samples : ms * 16u;
    int16_t pcm[512];
    while (miniapp_is_active() && generation == *current && position < total) {
        if ((score && *paused) || miniapp_platform_busy()) {
            if (!score)
                break;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const auto session = Session();
        sound_session = session;
        sound_interrupted = false;
        if (!Output().BeginPlayback(AudioPlaybackConfig{.session_id = session,
                                                        .recovery_prebuffer_bytes = 1024,
                                                        .max_buffered_pcm_bytes = 2048,
                                                        .producer_resume_pcm_bytes = 1024}))
            break;
        if (score)
            miniapp_synth_seek(&synth, score, position);
        while (miniapp_is_active() && generation == *current && !sound_interrupted &&
               !(score && *paused) && !miniapp_platform_busy()) {
            size_t count;
            if (score) {
                count = miniapp_synth_render(&synth, score, pcm, 512);
                position = synth.cursor;
                if (!count && loop) {
                    position = 0;
                    miniapp_synth_init(&synth);
                    continue;
                }
            } else {
                count = std::min<uint32_t>(512, total - position);
                for (size_t i = 0; i < count; ++i) {
                    phase = (phase + hz) % 16000u;
                    pcm[i] = phase < 8000u ? MINIAPP_TONE_AMPLITUDE : -MINIAPP_TONE_AMPLITUDE;
                }
                position += count;
            }
            if (!count)
                break;
            if (!Output().PushPcm(reinterpret_cast<uint8_t *>(pcm), count * 2, 16000,
                                  AudioOutputEvent{.session_id = session}, true))
                break;
        }
        const bool interrupted = sound_interrupted || (score && *paused) || miniapp_platform_busy();
        if (interrupted) {
            const auto queued = Output().BufferedPlaybackBytes(session) / 2;
            position -= std::min<uint32_t>(position, queued);
        } else {
            Output().MarkPlaybackFinished(AudioOutputEvent{.session_id = session});
            Output().WaitForPlaybackComplete(session);
        }
        Output().ClearActivePlaybackQueue(session);
        sound_session = 0;
        if (!score || !interrupted)
            break;
    }
    sound_active = false;
}
