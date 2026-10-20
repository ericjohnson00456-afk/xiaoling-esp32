#include "music_intent.h"

#include "application.h"
#include "playback_intent_manager.h"
#include "system/psram_task.h"
#include "ui/ui.h"
#include "xiaoling/conn/xiaoling_config.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <new>
#include <utility>

namespace {

constexpr char kTag[] = "MusicIntent";

struct KuwoUrlTaskContext {
  MusicIntent *intent;
  std::string item_id;
  std::string host;
  AudioPlayer::MusicUrlResolveCallback callback;
  bool external_stack = false;
};

} // namespace

void MusicIntent::Initialize(PlaybackIntentManager &intent_manager,
                             Application &app) {
  app_ = &app;
  intent_manager_ = &intent_manager;
  app_->audio_player_.SetMusicTrackCallback([](MusicTrack track) {
    Ui::SetMusicTrack(std::move(track));
  });
}

bool MusicIntent::SubmitKuwoList(std::vector<MusicTrack> tracks) {
  if (!Ready() || tracks.empty()) {
    return false;
  }
  user_paused_ = false;
  SetPendingKuwoList(std::move(tracks));
  if (!intent_manager_->Reenter(PlaybackIntentId::kMusic)) {
    ClearPendingList();
    return false;
  }
  source_ = MusicSource::kKuwoOnline;
  Ui::SetMusicPaused(false);
  ReturnToStandby();
  ESP_LOGI(kTag, "酷我音乐列表已排队，等待当前 TTS 播放完成");
  return true;
}

bool MusicIntent::SubmitTfCardList(std::vector<MusicTrack> tracks,
                                   std::size_t start_index) {
  if (!Ready() || tracks.empty()) {
    return false;
  }
  user_paused_ = false;
  const std::size_t effective_start_index =
      start_index < tracks.size() ? start_index : 0;
  SetPendingTfCardList(std::move(tracks), effective_start_index);
  if (!intent_manager_->Reenter(PlaybackIntentId::kMusic)) {
    ClearPendingList();
    return false;
  }
  source_ = MusicSource::kTfCard;
  Ui::SetMusicPaused(false);
  ReturnToStandby();
  ESP_LOGI(kTag, "TF 卡音乐列表已排队，从第 %u 首开始播放",
           static_cast<unsigned>(effective_start_index));
  return true;
}

bool MusicIntent::SubmitUrlList(std::vector<std::string> urls) {
  if (!Ready() || urls.empty()) {
    return false;
  }
  user_paused_ = false;
  SetPendingList(PendingListType::kUrls, std::move(urls));
  if (!intent_manager_->Reenter(PlaybackIntentId::kMusic)) {
    ClearPendingList();
    return false;
  }
  source_ = MusicSource::kUnknown;
  Ui::SetMusicPaused(false);
  ReturnToStandby();
  ESP_LOGI(kTag, "音乐 URL 列表已排队，等待当前 TTS 播放完成");
  return true;
}

void MusicIntent::Exit() {
  if (Ready()) {
    intent_manager_->Pop(PlaybackIntentId::kMusic);
  }
}

bool MusicIntent::Ready() const {
  return app_ != nullptr && intent_manager_ != nullptr;
}

void MusicIntent::ReturnToStandby() {
  app_->iat_utterance_active_ = false;
  app_->audio_service_.EnableAudioUpload(false);
  app_->session_controller_.EndSession();
  if (app_->GetDeviceState() != kDeviceStateIdle) {
    app_->SetDeviceState(kDeviceStateIdle);
  } else {
    Ui::SetStatus(Ui::Status::Standby);
    Ui::SetEmotion("待机");
  }
  ESP_LOGI(kTag, "音乐工具已停止录音并回到待命，等待语音事件自然结束");
}

void MusicIntent::SetUserPaused(bool paused) {
  const bool should_resume = user_paused_ && !paused;
  user_paused_ = paused;
  if (should_resume && intent_manager_->Top() == PlaybackIntentId::kMusic) {
    OnResumed();
  }
  Ui::SetMusicPaused(paused);
}

void MusicIntent::OnTfCardRemoved() {
  if (source_ != MusicSource::kTfCard) {
    return;
  }
  ESP_LOGI(kTag, "TF 卡已拔出，退出本地音乐事件");
  Exit();
}

void MusicIntent::SetPendingList(PendingListType type,
                                 std::vector<std::string> items) {
  pending_list_type_ = type;
  pending_list_ = std::move(items);
  pending_tracks_.clear();
  pending_start_index_ = 0;
}

void MusicIntent::SetPendingKuwoList(std::vector<MusicTrack> tracks) {
  pending_list_type_ = PendingListType::kKuwoItemIds;
  pending_list_.clear();
  pending_tracks_ = std::move(tracks);
  pending_start_index_ = 0;
}

void MusicIntent::SetPendingTfCardList(std::vector<MusicTrack> tracks,
                                       std::size_t start_index) {
  pending_list_type_ = PendingListType::kTfCardFiles;
  pending_list_.clear();
  pending_tracks_ = std::move(tracks);
  pending_start_index_ = start_index < pending_tracks_.size() ? start_index : 0;
}

void MusicIntent::ClearPendingList() {
  pending_list_type_ = PendingListType::kNone;
  pending_list_.clear();
  pending_tracks_.clear();
  pending_start_index_ = 0;
}

bool MusicIntent::StartPendingList() {
  const PendingListType type = pending_list_type_;
  std::vector<std::string> items = std::move(pending_list_);
  std::vector<MusicTrack> tracks = std::move(pending_tracks_);
  const std::size_t start_index = pending_start_index_;
  ClearPendingList();

  if (type == PendingListType::kKuwoItemIds) {
    return app_->audio_player_.PlayMusicItemList(
        tracks, [this](const std::string &item_id,
                       AudioPlayer::MusicUrlResolveCallback callback) {
          return QueueKuwoUrlResolve(item_id, std::move(callback));
        });
  }
  if (type == PendingListType::kTfCardFiles) {
    return app_->audio_player_.PlayMusicItemList(
        tracks,
        [](const std::string &path,
           AudioPlayer::MusicUrlResolveCallback callback) {
          if (path.empty() || !callback) return false;
          callback(true, path);
          return true;
        },
        start_index);
  }
  if (type == PendingListType::kUrls) {
    return app_->audio_player_.PlayMusicList(items) != 0;
  }
  return false;
}

bool MusicIntent::QueueKuwoUrlResolve(
    const std::string &item_id, AudioPlayer::MusicUrlResolveCallback callback) {
  if (item_id.empty() || !callback) {
    return false;
  }
  auto *context = new (std::nothrow) KuwoUrlTaskContext();
  if (context == nullptr) {
    return false;
  }
  context->intent = this;
  context->item_id = item_id;
  context->callback = std::move(callback);

  // Configuration reads must happen on the internal-RAM Application task.
  // The resolver itself is deliberately kept on a PSRAM worker because it
  // performs HTTP/TLS work and can use a large temporary stack.
  app_->Schedule([context]() {
    context->host = XiaolingConfig::GetHost();
    const BaseType_t task_result = PsramTask::Create(
        ResolveKuwoUrlTask, "kuwo_resolve", 8192, context, 4, nullptr,
        &context->external_stack);
    if (task_result == pdPASS) {
      return;
    }

    ESP_LOGE(kTag, "创建酷我 URL 解析任务失败");
    auto callback = std::move(context->callback);
    delete context;
    callback(false, {});
  });
  return true;
}

void MusicIntent::ResolveKuwoUrlTask(void *arg) {
  std::unique_ptr<KuwoUrlTaskContext> context(
      static_cast<KuwoUrlTaskContext *>(arg));
  const bool external_stack = context->external_stack;
  auto *intent = context->intent;
  auto *app = intent->app_;
  std::string url;
  bool success = false;
  {
    std::lock_guard<std::mutex> lock(app->xiaoling_conn_mutex_);
    success = app->xiaoling_conn_ != nullptr &&
              app->xiaoling_conn_->ResolveKuwoMusicUrl(
                  context->item_id, context->host, url);
  }
  app->Schedule([callback = std::move(context->callback), success,
                 url = std::move(url)]() mutable {
    callback(success, std::move(url));
  });
  context.reset();
  PsramTask::Delete(external_stack);
}

void MusicIntent::OnEnter() {
  if (StartPendingList()) {
    ESP_LOGI(kTag, "音乐事件进入，播放列表已缓存并开始播放");
    return;
  }
  if (!user_paused_ && app_->audio_player_.ResumeMusic()) {
    ESP_LOGI(kTag, "音乐事件进入，恢复当前音乐");
    return;
  }
  ESP_LOGW(kTag, "音乐事件进入，但没有可提交或恢复的播放列表");
}

void MusicIntent::OnPreempted() {
  if (!app_->audio_player_.PauseMusic()) {
    ESP_LOGW(kTag, "音乐事件被抢占，但暂停音乐失败");
  } else {
    ESP_LOGI(kTag, "音乐事件被高优先级事件抢占，音乐已暂停");
  }
}

void MusicIntent::OnResumed() {
  if (user_paused_) {
    ESP_LOGI(kTag, "音乐由用户暂停，跳过自动恢复");
    return;
  }
  if (app_->audio_player_.ResumeMusic()) {
    ESP_LOGI(kTag, "音乐事件恢复播放");
  } else {
    ESP_LOGW(kTag, "音乐事件恢复失败，当前曲目可能已结束");
  }
}

void MusicIntent::OnExit() {
  app_->audio_player_.ClearMusicList();
  ClearPendingList();
  source_ = MusicSource::kUnknown;
  user_paused_ = false;
  Ui::SetMusicPaused(false);
  ESP_LOGI(kTag, "音乐事件退出，播放列表已清除");
}
