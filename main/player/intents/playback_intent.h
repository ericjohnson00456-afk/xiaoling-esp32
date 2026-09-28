#pragma once

#include <cstdint>

// 数值越大，播放优先级越高。
enum class PlaybackIntentId : uint8_t {
  kNone = 0,
  kMusic,
  kVoiceSession,
  kPromptTone,
  kAlarm,
  kCount,
};
