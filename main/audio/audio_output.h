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
    // 有首块 PCM 后最多等待多久；0 表示沿用纯字节门槛（本地音乐）。
    uint32_t prebuffer_max_wait_ms = 0;
    // 输出队列短暂为空时先等待后续帧，不立即进入恢复预缓冲；0 保留旧策略。
    uint32_t underrun_grace_ms = 0;
    // 为 0 时仅使用通用任务数上限；非 0 时同时按当前排队 PCM 字节数反压。
    size_t max_buffered_pcm_bytes = 0;
    // 达到上限后的生产者需等待缓存回落到该水位，避免在边界反复唤醒。
    size_t producer_resume_pcm_bytes = 0;
};

// 16 kHz、单声道、16-bit PCM 每秒约 32 KiB。
constexpr size_t kAudioPlayerInitialPrebufferBytes = 16 * 1024;
// 云端分块 TTS 实测生成速度低于播放速度。优先积累约 3 秒 PCM；
// 短回复 EOF 时立即放行，避免为了水位等待不存在的后续数据。
constexpr size_t kAudioTtsInitialPrebufferBytes = 96 * 1024;
constexpr size_t kAudioTtsRecoveryPrebufferBytes = 64 * 1024;
constexpr uint32_t kAudioTtsPrebufferMaxWaitMs = 0;
constexpr uint32_t kAudioTtsUnderrunGraceMs = 60;
// 预缓冲门槛必须低于生产者上限，否则解码线程会被反压，导致永不首播。
constexpr size_t kAudioTtsMaxBufferedPcmBytes = 128 * 1024;
constexpr size_t kAudioTtsProducerResumeBytes = 112 * 1024;
// 本地提示音首块立即输出；欠载恢复也只等约一帧，不套用网络流缓冲。
constexpr size_t kAudioPromptInitialPrebufferBytes = 0;
constexpr size_t kAudioPromptRecoveryPrebufferBytes = 1152;
// TF 本地音乐用约 1 秒启动/恢复，并在 1.5～2 秒之间形成反压回差。
constexpr size_t kAudioLocalFilePrebufferBytes = 32 * 1024;
constexpr size_t kAudioLocalFileProducerResumeBytes = 48 * 1024;
constexpr size_t kAudioLocalFileMaxBufferedBytes = 64 * 1024;

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    virtual bool PushPcm(const uint8_t* data, size_t size, int sample_rate,
                         const AudioOutputEvent& output, bool wait) = 0;
    virtual bool BeginPlayback(const AudioPlaybackConfig& config) = 0;
    virtual void SuspendPlayback() = 0;
    virtual void ResumePlayback(const AudioOutputEvent& output) = 0;
    virtual void MarkPlaybackFinished(const AudioOutputEvent& output) = 0;
    // 0 表示无条件撤销；各后端停止/完成时必须传自己的会话 ID。
    virtual void ClearActivePlaybackQueue(uint32_t expected_session = 0) = 0;
    virtual void ClearPlaybackQueue() = 0;
    virtual void WaitForPlaybackComplete(uint32_t expected_session = 0) = 0;
    virtual uint32_t GetOutputLastActivityMs() const = 0;
    // 只返回指定活动会话排队的 PCM；旧后端不能读取新提示音的水位。
    virtual size_t BufferedPlaybackBytes(uint32_t expected_session) = 0;
};

#endif  // AUDIO_OUTPUT_H
