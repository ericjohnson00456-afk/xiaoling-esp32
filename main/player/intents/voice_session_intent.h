#pragma once

#include "playback_intent.h"

class Application;

class VoiceSessionIntent final {
public:
  void Initialize(Application &app);

  void OnEnter();
  void OnPreempted();
  void OnResumed();
  void OnExit();

private:
  Application *app_ = nullptr;
};
