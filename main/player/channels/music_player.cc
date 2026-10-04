#include "music_player.h"

#include "backend/playback_backend.h"
#include "tts_player.h"

#include <utility>

MusicPlayer::MusicPlayer(PlaybackBackend& engine, SessionAllocator allocate_session)
    : engine_(&engine), allocate_session_(std::move(allocate_session)) {}

uint32_t MusicPlayer::Submit(const PlaybackRequest& request, PlaybackBackend& engine) {
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
    session_id_ = session_id;
    paused_ = false;
    local_file_ = false;
    return session_id;
}

uint32_t MusicPlayer::PlayFile(const std::string& path) {
    if (path.empty()) {
        return 0;
    }
    const std::string uri = path.rfind("file://", 0) == 0
                                ? path
                                : "file://" + (path.front() == '/' ? path.substr(1) : path);
    PlaybackRequest request;
    request.session_id = allocate_session_();
    request.source_type = PlaybackSourceType::kFile;
    request.channel = PlaybackChannel::kMusic;
    request.source = uri;
    request.priority = 20;
    request.resumable = true;
    const auto session_id = Submit(request, *engine_);
    session_id_ = session_id;
    paused_ = false;
    local_file_ = session_id != 0;
    return session_id;
}

bool MusicPlayer::Pause() {
    if (!engine_->IsPlaying() || paused_) {
        return true;
    }
    // 暂停必须保留 GMF 的 HTTP/解码会话和当前进度；Stop 会结束会话，
    // 后续 Resume 只能重新提交 URL，导致音乐从头开始播放。
    if (!engine_->Pause()) {
        return false;
    }
    paused_ = true;
    return true;
}

bool MusicPlayer::Resume() {
    if (!paused_) {
        return engine_->IsPlaying();
    }
    if (!engine_->IsPlaying()) {
        paused_ = false;
        return false;
    }
    if (!engine_->Resume()) {
        return false;
    }
    paused_ = false;
    return true;
}

bool MusicPlayer::IsPaused() const {
    return paused_;
}

bool MusicPlayer::IsAudible() const {
    return engine_->IsPlaying() && !paused_;
}

bool MusicPlayer::IsLocalFilePlaying() const {
    return local_file_;
}

void MusicPlayer::Cancel(PlaybackBackend& engine) {
    engine.Stop(PlaybackEndReason::kCancelled);
    paused_ = false;
    local_file_ = false;
    session_id_ = 0;
}

void MusicPlayer::OnEvent(const PlaybackEvent& event) {
    if (event.session_id != session_id_ || event.channel != Type()) {
        return;
    }
    if (event.state == PlaybackState::kPaused) {
        paused_ = true;
    } else if (event.state == PlaybackState::kPlaying) {
        paused_ = false;
    } else if (event.state == PlaybackState::kStopped ||
        event.state == PlaybackState::kCompleted ||
        event.state == PlaybackState::kFailed) {
        paused_ = false;
        local_file_ = false;
        session_id_ = 0;
    }
}
