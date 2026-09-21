#pragma once

#include "playback_intent_stack.h"

#include <functional>

class AudioPlayer;

PlaybackIntentStack::Handlers CreateVoiceSessionIntentHandlers(
    AudioPlayer& audio_player, std::function<bool()> has_active_tts);
