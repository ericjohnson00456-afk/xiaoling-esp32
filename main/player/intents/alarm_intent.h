#pragma once

#include "playback_intent.h"

class PlaybackIntentManager;

class AlarmIntent final {
public:
  void Initialize(PlaybackIntentManager &intent_manager);

  void OnEnter();
  void OnPreempted();
  void OnResumed();
  void OnExit();

  bool Enter();
  bool Exit();

private:
  PlaybackIntentManager *intent_manager_ = nullptr;
  int previous_volume_ = -1;
};
