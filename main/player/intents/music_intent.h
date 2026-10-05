#pragma once

#include "playback_intent.h"
#include "player/audio_player.h"
#include "player/types/music_track.h"

#include <cstdint>
#include <string>
#include <vector>

class Application;
class PlaybackIntentManager;

class MusicIntent final {
public:
  void Initialize(PlaybackIntentManager &intent_manager, Application &app);

  void OnEnter();
  void OnPreempted();
  void OnResumed();
  void OnExit();

  bool SubmitKuwoList(std::vector<MusicTrack> tracks);
  bool SubmitTfCardList(std::vector<MusicTrack> tracks, std::size_t start_index);
  bool SubmitUrlList(std::vector<std::string> urls);
  // 音乐工具执行后停止录音并回到待命；TTS 和语音 Intent 自然收尾。
  void ReturnToStandby();
  // 用户主动暂停时禁止 Intent 自动恢复；清除后恢复当前列表索引。
  void SetUserPaused(bool paused);
  void Exit();

private:
  enum class PendingListType : uint8_t {
    kNone,
    kKuwoItemIds,
    kTfCardFiles,
    kUrls,
  };

  bool Ready() const;
  void SetPendingList(PendingListType type, std::vector<std::string> items);
  void SetPendingKuwoList(std::vector<MusicTrack> tracks);
  void SetPendingTfCardList(std::vector<MusicTrack> tracks,
                            std::size_t start_index);
  void ClearPendingList();
  bool StartPendingList();
  bool QueueKuwoUrlResolve(const std::string &item_id,
                           AudioPlayer::MusicUrlResolveCallback callback);
  static void ResolveKuwoUrlTask(void *arg);

  Application *app_ = nullptr;
  PlaybackIntentManager *intent_manager_ = nullptr;
  PendingListType pending_list_type_ = PendingListType::kNone;
  std::vector<std::string> pending_list_;
  std::vector<MusicTrack> pending_tracks_;
  std::size_t pending_start_index_ = 0;
  bool user_paused_ = false;
};
