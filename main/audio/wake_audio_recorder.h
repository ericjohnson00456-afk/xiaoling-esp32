#pragma once

#include "wake_recording_buffer.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>

struct WakeAudioRecordingStatus {
    bool recording = false;
    bool finishing = false;
    uint32_t data_bytes = 0;
    size_t pending_samples = 0;
    std::string path;
    std::string error;
};

// Optional diagnostic tap: no filesystem access or allocation on the model task.
class WakeAudioRecorder {
public:
    static WakeAudioRecorder& GetInstance();

    bool Start();
    bool Stop();  // Drains queued PCM, finalizes WAV, closes the file.
    WakeAudioRecordingStatus GetStatus();
    void SetCardAvailable(bool available);
    void Feed(const int16_t* pcm, size_t samples);

private:
    WakeAudioRecorder() = default;
    bool StopLocked();
    void WriterLoop();

    // Serialize start/stop/card removal, but never hold mutex_ across SD I/O.
    std::mutex control_mutex_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic_bool recording_{false};
    bool writer_running_ = false;
    bool card_available_ = true;
    uint32_t accepted_bytes_ = 0;
    uint32_t written_bytes_ = 0;
    uint32_t next_file_number_ = 1;
    std::string path_;
    const char* error_ = nullptr;
    FILE* file_ = nullptr;
    int16_t* storage_ = nullptr;
    WakeRecordingBuffer buffer_;
};
