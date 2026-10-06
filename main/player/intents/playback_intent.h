#pragma once

#include <cstdint>

// 数值越大，播放优先级越高。
enum class PlaybackIntentId : uint8_t {
  kNone = 0,
  kMusic,
  kVoiceSession,
  kCameraCapture,
  kAlarm,
  // 仅由需要提示音业务语义的流程显式入栈；普通短音效仍沿用所属业务 Intent。
  kPromptTone,
  kCount,
};
