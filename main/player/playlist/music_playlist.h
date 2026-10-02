#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "types/playback_event.h"

// 管理音乐播放列表和条目 URL 解析，不负责创建或控制具体播放后端。
class MusicPlaylist {
public:
    using MusicUrlResolveCallback = std::function<void(bool, std::string)>;
    using MusicUrlResolver =
        std::function<bool(const std::string&, MusicUrlResolveCallback)>;
    using StartPlaybackCallback = std::function<uint32_t(const std::string&)>;

    explicit MusicPlaylist(StartPlaybackCallback start_playback);

    uint32_t PlayUrl(const std::string& url);
    uint32_t PlayUrlList(const std::vector<std::string>& urls);
    bool PlayItemList(const std::vector<std::string>& item_ids,
                      MusicUrlResolver resolver);
    bool PlayPrevious(bool play_when_ready = true);
    bool PlayNext(bool play_when_ready = true);
    bool PlayCurrent(bool play_when_ready = true);
    bool Replay(bool play_when_ready = true);
    void Clear();
    bool HasList() const;
    bool IsAtLast() const;

    // 由 AudioPlayer 在音乐通道收到事件后调用。
    void HandlePlaybackEvent(const PlaybackEvent& event);

    uint32_t CurrentSessionId() const;
    bool HasCurrentSession() const;

private:
    enum class NaturalEndResult {
        kIgnored,
        kNextStarted,
        kReachedEnd,
    };

    struct MusicListItem {
        std::string item_id;
        std::string url;
        bool resolving = false;
        bool play_when_resolved = false;
    };

    bool RequestAtIndex(std::size_t index, bool play_when_ready);
    void HandleUrlResolved(uint32_t generation, std::size_t index,
                           bool success, std::string url);
    bool StartResolved(std::size_t index, const std::string& url);
    void PrefetchNext(std::size_t current_index);
    NaturalEndResult PlayNextAfterNaturalEnd(uint32_t session_id);

    StartPlaybackCallback start_playback_;
    mutable std::mutex mutex_;
    std::vector<MusicListItem> items_;
    std::size_t current_index_ = 0;
    std::size_t active_index_ = static_cast<std::size_t>(-1);
    uint32_t generation_ = 0;
    uint32_t current_session_id_ = 0;
    bool current_retry_used_ = false;
    MusicUrlResolver resolver_;
};
