#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class HomePresenter;

// 负责把 IAT/TTS 事件整理成首页可显示的实时文本和同步字幕。
class VoiceSubtitleController {
public:
    explicit VoiceSubtitleController(HomePresenter& presenter);

    void Start();
    void Stop();

private:
    struct SubtitleSegment {
        uint32_t cloud_index = 0;
        std::string text;
        uint32_t start_ms = 0;
        uint32_t end_ms = 0;
    };

    void QueueIatText(std::string text);
    void DrainIatText();
    void QueueTtsAudioProgress(uint32_t audio_clock_ms);
    void OnTtsTextStart();
    void OnTtsTextUpdate(const std::string& text);
    void OnTtsTextInterrupt();
    void OnTtsPlaying();
    void OnTtsAudioProgress(uint32_t audio_clock_ms);
    void OnTtsStopped();
    void ResetSubtitles(bool clear_text);
    void ScheduleSubtitle();

    HomePresenter& presenter_;
    std::vector<uint32_t> event_subscriptions_;
    std::vector<SubtitleSegment> subtitle_segments_;
    size_t subtitle_index_ = 0;
    bool subtitle_audio_started_ = false;
    uint32_t subtitle_audio_clock_ms_ = 0;
    std::atomic<uint32_t> tts_progress_generation_{0};
    std::atomic<uint32_t> latest_tts_audio_clock_ms_{0};
    std::atomic_bool tts_progress_dispatch_pending_{false};
    std::mutex iat_text_mutex_;
    std::string pending_iat_text_;
    int64_t pending_iat_received_us_ = 0;
    int64_t last_iat_delay_log_us_ = 0;
    uint32_t last_voice_text_revision_ = 0;
    bool iat_dispatch_pending_ = false;
    bool iat_text_visible_ = false;
};
