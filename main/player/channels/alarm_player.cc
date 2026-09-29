#include "alarm_player.h"

#include "engine/player_engine.h"
#include "resource/resource.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace {

constexpr char kTag[] = "AlarmPlayer";

bool IsTerminal(const PlaybackEvent& event) {
    return event.state == PlaybackState::kCompleted ||
           event.state == PlaybackState::kStopped ||
           event.state == PlaybackState::kFailed;
}

bool IsHttpUrl(const std::string& url) {
    const size_t scheme_size = url.rfind("https://", 0) == 0 ? 8 :
                               (url.rfind("http://", 0) == 0 ? 7 : 0);
    if (scheme_size == 0 || scheme_size >= url.size()) {
        return false;
    }
    const auto authority_end = url.find_first_of("/?#", scheme_size);
    const auto authority_size =
        (authority_end == std::string::npos ? url.size() : authority_end) - scheme_size;
    if (authority_size == 0) {
        return false;
    }
    return std::none_of(url.begin(), url.end(), [](unsigned char ch) {
        return std::iscntrl(ch) != 0 || std::isspace(ch) != 0;
    });
}

}  // namespace

AlarmPlayer::AlarmPlayer(PlayerEngine& engine, SessionAllocator allocate_session)
    : engine_(&engine), allocate_session_(std::move(allocate_session)) {}

uint32_t AlarmPlayer::Submit(const PlaybackRequest& request, PlayerEngine& engine) {
    if (!engine.Play(request)) {
        session_id_ = 0;
        return 0;
    }
    session_id_ = request.session_id;
    return session_id_;
}

uint32_t AlarmPlayer::PlayTone(uint16_t tone_id) {
    ResourceData resource;
    if (!Resource::GetTone(tone_id, resource)) {
        ESP_LOGW(kTag, "闹钟 tone 不存在: id=%u", static_cast<unsigned>(tone_id));
        return 0;
    }
    PlaybackRequest request;
    request.session_id = allocate_session_();
    request.source_type = PlaybackSourceType::kRomfs;
    request.channel = PlaybackChannel::kAlarm;
    request.source = "embed://tone/" + std::to_string(tone_id) + "_tone.mp3";
    request.priority = 100;
    return Submit(request, *engine_);
}

uint32_t AlarmPlayer::PlayUrl(const std::string& url) {
    if (!IsHttpUrl(url)) {
        ESP_LOGE(kTag, "拒绝无效的闹钟音频 URL, bytes=%u",
                 static_cast<unsigned>(url.size()));
        return 0;
    }
    PlaybackRequest request;
    request.session_id = allocate_session_();
    request.source_type = PlaybackSourceType::kUrl;
    request.channel = PlaybackChannel::kAlarm;
    request.source = url;
    request.priority = 100;
    return Submit(request, *engine_);
}

void AlarmPlayer::Cancel(PlayerEngine& engine) {
    engine.Stop(PlaybackEndReason::kCancelled);
}

void AlarmPlayer::OnEvent(const PlaybackEvent& event) {
    if (event.channel != PlaybackChannel::kAlarm ||
        event.session_id != session_id_) {
        return;
    }
    if (IsTerminal(event)) {
        // 回调可能立即提交下一阶段，必须先清理旧 session。
        session_id_ = 0;
    }
    if (event_callback_) {
        event_callback_(event);
    }
}

void AlarmPlayer::SetEventCallback(EventCallback callback) {
    event_callback_ = std::move(callback);
}
