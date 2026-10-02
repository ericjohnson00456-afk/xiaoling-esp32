#include "music_playlist.h"

#include "channels/tts_player.h"

#include <esp_log.h>

#include <utility>

#define TAG "MusicPlaylist"

MusicPlaylist::MusicPlaylist(StartPlaybackCallback start_playback)
    : start_playback_(std::move(start_playback)) {}

uint32_t MusicPlaylist::PlayUrl(const std::string& url) {
    if (!TtsPlayer::IsHttpUrl(url)) {
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
        items_.push_back(MusicListItem{.url = url});
        current_index_ = 0;
        active_index_ = static_cast<std::size_t>(-1);
        ++generation_;
        current_session_id_ = 0;
        current_retry_used_ = false;
        resolver_ = nullptr;
    }
    if (!StartResolved(0, url)) {
        Clear();
        return 0;
    }
    return CurrentSessionId();
}

uint32_t MusicPlaylist::PlayUrlList(const std::vector<std::string>& urls) {
    std::vector<MusicListItem> valid_urls;
    valid_urls.reserve(urls.size());
    for (const auto& url : urls) {
        if (TtsPlayer::IsHttpUrl(url)) {
            valid_urls.push_back(MusicListItem{.url = url});
        }
    }
    if (valid_urls.empty()) {
        return 0;
    }

    std::string first_url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_ = std::move(valid_urls);
        current_index_ = 0;
        active_index_ = static_cast<std::size_t>(-1);
        first_url = items_.front().url;
        ++generation_;
        current_session_id_ = 0;
        current_retry_used_ = false;
        resolver_ = nullptr;
    }
    if (!StartResolved(0, first_url)) {
        Clear();
        return 0;
    }
    return CurrentSessionId();
}

bool MusicPlaylist::PlayItemList(const std::vector<std::string>& item_ids,
                                 MusicUrlResolver resolver) {
    if (!resolver) {
        return false;
    }
    std::vector<MusicListItem> items;
    items.reserve(item_ids.size());
    for (const auto& item_id : item_ids) {
        if (!item_id.empty()) {
            items.push_back(MusicListItem{.item_id = item_id});
        }
    }
    if (items.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_ = std::move(items);
        current_index_ = 0;
        active_index_ = static_cast<std::size_t>(-1);
        ++generation_;
        current_session_id_ = 0;
        current_retry_used_ = false;
        resolver_ = std::move(resolver);
    }
    if (!RequestAtIndex(0, true)) {
        Clear();
        return false;
    }
    return true;
}

bool MusicPlaylist::PlayPrevious(bool play_when_ready) {
    std::size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) {
            return false;
        }
        // 第一首没有上一首，保持索引不变，重新播放当前歌曲。
        if (current_index_ != 0) {
            --current_index_;
        }
        active_index_ = static_cast<std::size_t>(-1);
        index = current_index_;
        current_retry_used_ = false;
    }
    return RequestAtIndex(index, play_when_ready);
}

bool MusicPlaylist::PlayNext(bool play_when_ready) {
    std::size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) {
            return false;
        }
        if (current_index_ + 1 >= items_.size()) {
            // 播放列表是顺序模式；最后一首之后不回到第一首。
            return false;
        }
        ++current_index_;
        active_index_ = static_cast<std::size_t>(-1);
        index = current_index_;
        current_retry_used_ = false;
    }
    return RequestAtIndex(index, play_when_ready);
}

bool MusicPlaylist::PlayCurrent(bool play_when_ready) {
    std::size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) {
            return false;
        }
        index = current_index_;
        active_index_ = static_cast<std::size_t>(-1);
        current_retry_used_ = false;
    }
    return RequestAtIndex(index, play_when_ready);
}

bool MusicPlaylist::Replay(bool play_when_ready) {
    std::size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) {
            return false;
        }
        index = current_index_;
        active_index_ = static_cast<std::size_t>(-1);
        current_retry_used_ = false;
    }
    return RequestAtIndex(index, play_when_ready);
}

void MusicPlaylist::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    current_index_ = 0;
    active_index_ = static_cast<std::size_t>(-1);
    ++generation_;
    current_session_id_ = 0;
    current_retry_used_ = false;
    resolver_ = nullptr;
}

bool MusicPlaylist::HasList() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !items_.empty();
}

bool MusicPlaylist::IsAtLast() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !items_.empty() && current_index_ + 1 >= items_.size();
}

bool MusicPlaylist::RequestAtIndex(std::size_t index, bool play_when_ready) {
    MusicUrlResolver resolver;
    std::string item_id;
    std::string cached_url;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= items_.size()) {
            return false;
        }
        auto& item = items_[index];
        if (!item.url.empty()) {
            cached_url = item.url;
        } else {
            if (!resolver_ || item.item_id.empty()) {
                return false;
            }
            item.play_when_resolved = item.play_when_resolved || play_when_ready;
            if (item.resolving) {
                return true;
            }
            item.resolving = true;
            resolver = resolver_;
            item_id = item.item_id;
            generation = generation_;
        }
    }

    if (!cached_url.empty()) {
        return !play_when_ready || StartResolved(index, cached_url);
    }

    const bool submitted = resolver(
        item_id, [this, generation, index](bool success, std::string url) mutable {
            HandleUrlResolved(generation, index, success, std::move(url));
        });
    if (!submitted) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == generation_ && index < items_.size()) {
            items_[index].resolving = false;
            items_[index].play_when_resolved = false;
        }
    }
    return submitted;
}

void MusicPlaylist::HandleUrlResolved(uint32_t generation, std::size_t index,
                                      bool success, std::string url) {
    const bool valid_url = success && TtsPlayer::IsHttpUrl(url);
    bool should_play = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || index >= items_.size()) {
            return;
        }
        auto& item = items_[index];
        item.resolving = false;
        if (valid_url) {
            item.url = url;
        }
        should_play = valid_url && item.play_when_resolved && current_index_ == index;
        item.play_when_resolved = false;
    }
    if (should_play) {
        if (!StartResolved(index, url)) {
            ESP_LOGW(TAG, "解析后的音乐 URL 提交播放失败");
        }
    } else if (!valid_url) {
        ESP_LOGW(TAG, "音乐条目 URL 解析失败: index=%u", static_cast<unsigned>(index));
    }
}

bool MusicPlaylist::StartResolved(std::size_t index, const std::string& url) {
    if (!start_playback_) {
        return false;
    }
    const uint32_t session_id = start_playback_(url);
    if (session_id == 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= items_.size() || current_index_ != index) {
            return false;
        }
        current_session_id_ = session_id;
        active_index_ = index;
    }
    PrefetchNext(index);
    return true;
}

void MusicPlaylist::PrefetchNext(std::size_t current_index) {
    std::size_t next_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.size() < 2 || current_index >= items_.size()) {
            return;
        }
        next_index = (current_index + 1) % items_.size();
    }
    RequestAtIndex(next_index, false);
}

void MusicPlaylist::HandlePlaybackEvent(const PlaybackEvent& event) {
    if (event.state == PlaybackState::kFailed) {
        std::size_t retry_index = 0;
        bool should_retry = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (event.session_id == current_session_id_ &&
                current_index_ < items_.size() &&
                !items_[current_index_].item_id.empty() && !current_retry_used_) {
                retry_index = current_index_;
                auto& item = items_[retry_index];
                item.url.clear();
                item.resolving = false;
                item.play_when_resolved = false;
                current_retry_used_ = true;
                should_retry = true;
            }
        }
        if (should_retry) {
            ESP_LOGW(TAG, "音乐流播放失败，重新解析当前条目 URL");
            RequestAtIndex(retry_index, true);
        }
    }
    if (event.state == PlaybackState::kCompleted &&
        event.reason == PlaybackEndReason::kNaturalEnd) {
        if (PlayNextAfterNaturalEnd(event.session_id) ==
            NaturalEndResult::kReachedEnd) {
            // 让上层通过音乐终态事件识别“列表已结束”，并弹出 MusicIntent。
            Clear();
        }
    }
}

MusicPlaylist::NaturalEndResult MusicPlaylist::PlayNextAfterNaturalEnd(
    uint32_t session_id) {
    std::size_t next_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_id != current_session_id_) {
            return NaturalEndResult::kIgnored;
        }
        if (current_index_ + 1 >= items_.size()) {
            return NaturalEndResult::kReachedEnd;
        }
        ++current_index_;
        active_index_ = static_cast<std::size_t>(-1);
        next_index = current_index_;
        current_retry_used_ = false;
    }
    ESP_LOGI(TAG, "音乐自然结束，自动播放下一首: index=%u",
             static_cast<unsigned>(next_index));
    return RequestAtIndex(next_index, true)
               ? NaturalEndResult::kNextStarted
               : NaturalEndResult::kIgnored;
}

uint32_t MusicPlaylist::CurrentSessionId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_session_id_;
}

bool MusicPlaylist::HasCurrentSession() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !items_.empty() && active_index_ == current_index_ &&
           current_session_id_ != 0;
}
