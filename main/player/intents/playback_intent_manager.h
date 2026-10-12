#pragma once

#include "alarm_intent.h"
#include "camera_capture_intent.h"
#include "music_intent.h"
#include "playback_intent.h"
#include "voice_session_intent.h"

#include <array>
#include <cstddef>
#include <functional>

class Application;

// 一个很小的优先级栈：高优先级事件入栈时暂停栈顶，退出时自动恢复下一项。
class PlaybackIntentManager final {
public:
  void Initialize(Application &app);

  bool Push(PlaybackIntentId id);
  bool Reenter(PlaybackIntentId id);
  bool Pop(PlaybackIntentId id);
  bool Contains(PlaybackIntentId id) const;
  // 只有未被用户暂停的音乐才参与语音会话的抢占和自动恢复策略。
  bool HasUnpausedMusic() const {
    return Contains(PlaybackIntentId::kMusic) && !music_.IsUserPaused();
  }
  PlaybackIntentId Top() const;
  void Clear();

  MusicIntent &Music() { return music_; }
  AlarmIntent &Alarm() { return alarm_; }
  CameraCaptureIntent &CameraCapture() { return camera_capture_; }

private:
  struct Callbacks {
    std::function<void()> enter;
    std::function<void()> preempt;
    std::function<void()> resume;
    std::function<void()> exit;
  };

  static constexpr size_t kCount =
      static_cast<size_t>(PlaybackIntentId::kCount);

  template <typename T> void Bind(PlaybackIntentId id, T &intent) {
    callbacks_[Index(id)] = {
        [&intent]() { intent.OnEnter(); },
        [&intent]() { intent.OnPreempted(); },
        [&intent]() { intent.OnResumed(); },
        [&intent]() { intent.OnExit(); },
    };
  }

  static size_t Index(PlaybackIntentId id) { return static_cast<size_t>(id); }
  static bool IsValid(PlaybackIntentId id) {
    return id > PlaybackIntentId::kNone && id < PlaybackIntentId::kCount;
  }

  MusicIntent music_;
  VoiceSessionIntent voice_session_;
  CameraCaptureIntent camera_capture_;
  AlarmIntent alarm_;

  std::array<PlaybackIntentId, kCount> stack_{};
  std::array<Callbacks, kCount> callbacks_{};
  std::array<bool, kCount> entered_{};
  size_t size_ = 0;
};
