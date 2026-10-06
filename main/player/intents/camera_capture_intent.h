#pragma once

#include "playback_intent.h"

#include <atomic>
#include <functional>

class Application;
class PlaybackIntentManager;

// 拍照业务在播放优先级栈中的薄适配层。这里只处理音乐、语音、提示音和
// 闹钟之间的资源抢占，不承载摄像头采集、云端请求或 UI 状态机。
class CameraCaptureIntent final {
public:
  using CancelCallback = std::function<void()>;

  void Initialize(PlaybackIntentManager &intent_manager, Application &app);
  void SetCancelCallback(CancelCallback callback);

  bool Enter();
  void Exit();
  bool active() const { return active_.load(); }

  void OnEnter();
  void OnPreempted();
  void OnResumed();
  void OnExit();

private:
  void CancelCapture();

  Application *app_ = nullptr;
  PlaybackIntentManager *intent_manager_ = nullptr;
  CancelCallback cancel_callback_;
  std::atomic_bool active_{false};
  std::atomic_bool exit_pending_{false};
  std::atomic_bool cancel_pending_{false};
};
