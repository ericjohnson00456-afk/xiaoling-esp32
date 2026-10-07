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
    size_t recovery_prebuffer_bytes = 0;
    // 为 0 时仅使用通用任务数上限；非 0 时同时按当前排队 PCM 字节数反压。
    size_t max_buffered_pcm_bytes = 0;
    // 达到上限后的生产者需等待缓存回落到该水位，避免在边界反复唤醒。
    size_t producer_resume_pcm_bytes = 0;
};

// 16 kHz、单声道、16-bit PCM 每秒约 32 KiB。
constexpr size_t kAudioPlayerInitialPrebufferBytes = 16 * 1024;
constexpr size_t kAudioTtsInitialPrebufferBytes = 64 * 1024;
// TF 本地音乐用约 1 秒启动/恢复，并在 1.5～2 秒之间形成反压回差。
constexpr size_t kAudioLocalFilePrebufferBytes = 32 * 1024;
constexpr size_t kAudioLocalFileProducerResumeBytes = 48 * 1024;
constexpr size_t kAudioLocalFileMaxBufferedBytes = 64 * 1024;

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
