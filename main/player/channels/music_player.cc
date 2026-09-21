#include "music_player.h"

#include "engine/player_engine.h"
#include "tts_player.h"

#include <utility>

MusicPlayer::MusicPlayer(PlayerEngine& engine, SessionAllocator allocate_session)
    : engine_(&engine), allocate_session_(std::move(allocate_session)) {}

uint32_t MusicPlayer::Submit(const PlaybackRequest& request, PlayerEngine& engine) {
    return engine.Play(request) ? request.session_id : 0;
}

uint32_t MusicPlayer::PlayUrl(const std::string& url) {
    if (!TtsPlayer::IsHttpUrl(url)) {
        return 0;
    }
    PlaybackRequest request;
    request.session_id = allocate_session_();
    request.source_type = PlaybackSourceType::kUrl;
    request.channel = PlaybackChannel::kMusic;
    request.source = url;
    request.priority = 20;
    request.resumable = true;
    const auto session_id = Submit(request, *engine_);
    if (session_id != 0) {
        current_url_ = url;
    }
    session_id_ = session_id;
    paused_ = false;
    return session_id;
}

bool MusicPlayer::Pause() {
    if (!engine_->IsPlaying() || paused_) {
        return true;
    }
    if (!engine_->Stop(PlaybackEndReason::kPreempted)) {
        return false;
    }
    paused_ = true;
    return true;
}

bool MusicPlayer::Resume() {
    if (!paused_ || current_url_.empty() || PlayUrl(current_url_) == 0) {
        return false;
    }
    return true;
}

bool MusicPlayer::IsAudible() const {
    return engine_->IsPlaying() && !paused_;
}

void MusicPlayer::Cancel(PlayerEngine& engine) {
    engine.Stop(PlaybackEndReason::kCancelled);
    paused_ = false;
    session_id_ = 0;
}

void MusicPlayer::OnEvent(const PlaybackEvent& event) {
    if (event.session_id != session_id_ || event.channel != Type()) {
        return;
    }
    if (event.state == PlaybackState::kStopped ||
        event.state == PlaybackState::kCompleted ||
        event.state == PlaybackState::kFailed) {
        if (!paused_) {
            session_id_ = 0;
        }
    }
}
