#include "playback_intent_manager.h"

#include <algorithm>

void PlaybackIntentManager::Initialize(Application &app) {
  music_.Initialize(*this, app);
  voice_session_.Initialize(app);
  camera_capture_.Initialize(*this, app);
  alarm_.Initialize(*this);

  Bind(PlaybackIntentId::kMusic, music_);
  Bind(PlaybackIntentId::kVoiceSession, voice_session_);
  Bind(PlaybackIntentId::kCameraCapture, camera_capture_);
  Bind(PlaybackIntentId::kAlarm, alarm_);
}

bool PlaybackIntentManager::Push(PlaybackIntentId id) {
  if (!IsValid(id)) {
    return false;
  }
  if (Contains(id)) {
    return true;
  }
  if (size_ >= stack_.size()) {
    return false;
  }

  const PlaybackIntentId old_top = Top();

  // 按枚举值从低到高排列，数组末尾永远是当前最高优先级事件。
  size_t pos = size_;
  while (pos > 0 && stack_[pos - 1] > id) {
    stack_[pos] = stack_[pos - 1];
    --pos;
  }
  stack_[pos] = id;
  ++size_;

  // 只有栈顶变化时才需要暂停旧事件、启动新事件。
  if (Top() != old_top) {
    if (old_top != PlaybackIntentId::kNone) {
      if (callbacks_[Index(old_top)].preempt) {
        callbacks_[Index(old_top)].preempt();
      }
    }
    if (callbacks_[Index(id)].enter) {
      callbacks_[Index(id)].enter();
    }
    entered_[Index(id)] = true;
  }
  return true;
}

bool PlaybackIntentManager::Reenter(PlaybackIntentId id) {
  if (!IsValid(id)) {
    return false;
  }
  if (!Contains(id)) {
    return Push(id);
  }

  // 用于云端重新下发音乐列表：如果当前被 TTS 压住，等恢复到栈顶时
  // 执行 OnEnter；如果已经在栈顶，则立即重新进入。
  entered_[Index(id)] = false;
  if (Top() == id) {
    if (callbacks_[Index(id)].enter) {
      callbacks_[Index(id)].enter();
    }
    entered_[Index(id)] = true;
  }
  return true;
}

bool PlaybackIntentManager::Pop(PlaybackIntentId id) {
  if (!IsValid(id)) {
    return false;
  }
  const auto end = stack_.begin() + size_;
  const auto it = std::find(stack_.begin(), end, id);
  if (it == end) {
    return false;
  }

  const bool was_top = it == end - 1;
  if (callbacks_[Index(id)].exit) {
    callbacks_[Index(id)].exit();
  }
  entered_[Index(id)] = false;
  std::move(it + 1, end, it);
  --size_;

  // 栈顶事件退出后，自动恢复之前被暂停的播放事件。
  if (was_top && size_ > 0) {
    const PlaybackIntentId next = Top();
    if (entered_[Index(next)]) {
      if (callbacks_[Index(next)].resume) {
        callbacks_[Index(next)].resume();
      }
    } else {
      if (callbacks_[Index(next)].enter) {
        callbacks_[Index(next)].enter();
      }
      entered_[Index(next)] = true;
    }
  }
  return true;
}

bool PlaybackIntentManager::Contains(PlaybackIntentId id) const {
  return IsValid(id) && std::find(stack_.begin(), stack_.begin() + size_, id) !=
                            stack_.begin() + size_;
}

PlaybackIntentId PlaybackIntentManager::Top() const {
  return size_ == 0 ? PlaybackIntentId::kNone : stack_[size_ - 1];
}

void PlaybackIntentManager::Clear() {
  while (size_ > 0) {
    const PlaybackIntentId id = stack_[--size_];
    if (callbacks_[Index(id)].exit) {
      callbacks_[Index(id)].exit();
    }
    entered_[Index(id)] = false;
  }
}
