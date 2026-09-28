#pragma once

#include "playback_intent_stack.h"

#include <functional>

class AudioPlayer;

PlaybackIntentStack::Handlers CreateMusicIntentHandlers(
    AudioPlayer& audio_player, std::function<bool()> should_resume,
    std::function<bool()> start_playback,
    std::function<void()> clear_pending_list);
