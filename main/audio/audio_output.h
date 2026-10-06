#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <cstddef>
#include <cstdint>

// AudioService 对播放器暴露的最小物理输出接口。
// 这里不携带 TTS、音乐、提示音等业务通道信息。
struct AudioOutputEvent {
    uint32_t session_id = 0;
    uint32_t audio_clock_ms = 0;
};

struct AudioPlaybackConfig {
    uint32_t session_id = 0;
    size_t initial_prebuffer_bytes = 0;
};

// About 0.5 seconds of 16 kHz, mono, 16-bit PCM. SD and LCD may share an SPI
// bus, so local playback needs enough queued audio to absorb display traffic.
constexpr size_t kAudioPlayerInitialPrebufferBytes = 16 * 1024;
constexpr size_t kAudioTtsInitialPrebufferBytes = 64 * 1024;

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    virtual bool PushPcm(const uint8_t* data, size_t size, int sample_rate,
                         const AudioOutputEvent& output, bool wait) = 0;
    virtual void BeginPlayback(const AudioPlaybackConfig& config) = 0;
    virtual void SuspendPlayback() = 0;
    virtual void ResumePlayback(const AudioOutputEvent& output) = 0;
    virtual void MarkPlaybackFinished(const AudioOutputEvent& output) = 0;
    virtual void ClearActivePlaybackQueue() = 0;
    virtual void ClearPlaybackQueue() = 0;
    virtual void WaitForPlaybackComplete() = 0;
    virtual uint32_t GetOutputLastActivityMs() const = 0;
};

#endif  // AUDIO_OUTPUT_H
