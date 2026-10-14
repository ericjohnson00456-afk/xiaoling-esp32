#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "event/event_epoch.h"

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

    // 无 timeline 字幕的句段排期；每个节点到时整体替换上一句。
    struct PlainSubtitleCue {
        size_t start_byte = 0;
        size_t end_byte = 0;
        uint32_t show_at_ms = 0;
    };

    void QueueIatText(std::string text, AppEvent::EpochSnapshot scope);
    void DrainIatText();
    void QueueTtsAudioProgress(uint32_t audio_clock_ms);
    void OnTtsTextStart();
    void OnTtsTextUpdate(const std::string& text);
    void OnTtsTextEnd();
    void OnTtsTextInterrupt();
    void OnTtsPlaying();
    void OnTtsAudioProgress(uint32_t audio_clock_ms);
    void OnTtsStopped();
    void ResetSubtitles(bool clear_text);
    void ScheduleSubtitle();
    void AppendPlainSubtitle(const std::string& text);
    void BuildPlainSubtitleCues(bool flush_pending);
    void SchedulePlainSubtitle();

    HomePresenter& presenter_;
    std::vector<uint32_t> event_subscriptions_;
    std::vector<SubtitleSegment> subtitle_segments_;
    size_t subtitle_index_ = 0;
    bool subtitle_audio_started_ = false;
    uint32_t subtitle_audio_clock_ms_ = 0;
    std::string plain_subtitle_text_;
    std::vector<PlainSubtitleCue> plain_subtitle_cues_;
    size_t plain_subtitle_cue_index_ = 0;
    size_t plain_subtitle_visible_start_byte_ = 0;
    size_t plain_subtitle_visible_bytes_ = 0;
    size_t plain_subtitle_scheduled_bytes_ = 0;
    uint32_t plain_subtitle_next_due_ms_ = 0;
    bool plain_subtitle_mode_ = false;
    std::atomic<uint32_t> tts_progress_generation_{0};
    std::atomic<uint32_t> latest_tts_audio_clock_ms_{0};
    std::atomic_bool tts_progress_dispatch_pending_{false};
    std::mutex iat_text_mutex_;
    std::string pending_iat_text_;
    AppEvent::EpochSnapshot pending_iat_scope_;
    int64_t pending_iat_received_us_ = 0;
    int64_t last_iat_delay_log_us_ = 0;
    uint32_t last_voice_text_revision_ = 0;
    bool iat_dispatch_pending_ = false;
    bool iat_text_visible_ = false;
};
