#ifndef XIAOLING_TYPES_H
#define XIAOLING_TYPES_H

#include <cstdint>
#include <vector>

// 小聆音频上行和下行使用的音频包。
struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};

enum AbortReason {
    kAbortReasonNone,
    kAbortReasonWakeWordDetected
};

enum ListeningMode {
    kListeningModeAutoStop,
    kListeningModeManualStop,
    kListeningModeRealtime // 需要 AEC 支持
};

#endif // XIAOLING_TYPES_H
