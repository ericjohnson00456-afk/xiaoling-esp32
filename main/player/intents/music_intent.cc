#include "music_intent.h"

#include "application.h"
#include "playback_intent_manager.h"
#include "ui/ui_app.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <utility>

namespace {

constexpr char kTag[] = "MusicIntent";

struct KuwoUrlTaskContext {
  MusicIntent *intent;
  std::string item_id;
  AudioPlayer::MusicUrlResolveCallback callback;
};

} // namespace

void MusicIntent::Initialize(PlaybackIntentManager &intent_manager,
                             Application &app) {
  app_ = &app;
  intent_manager_ = &intent_manager;
}

bool MusicIntent::SubmitKuwoList(std::vector<std::string> item_ids) {
  if (!Ready() || item_ids.empty()) {
    return false;
  }
  user_paused_ = false;
  SetPendingList(PendingListType::kKuwoItemIds, std::move(item_ids));
  if (!intent_manager_->Reenter(PlaybackIntentId::kMusic)) {
    ClearPendingList();
    return false;
  }
  ReturnToStandby();
  ESP_LOGI(kTag, "酷我音乐列表已排队，等待当前 TTS 播放完成");
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
  app_->audio_service_.EnableVoiceProcessing(false);
  app_->session_controller_.EndSession();
  if (app_->GetDeviceState() != kDeviceStateIdle) {
    app_->SetDeviceState(kDeviceStateIdle);
  } else {
    UiApp::SetStatus(UiApp::Status::Standby);
    UiApp::SetEmotion("待机");
  }
  ESP_LOGI(kTag, "音乐工具已停止录音并回到待命，等待语音事件自然结束");
}

void MusicIntent::SetUserPaused(bool paused) {
  const bool should_resume = user_paused_ && !paused;
  user_paused_ = paused;
  if (should_resume && intent_manager_->Top() == PlaybackIntentId::kMusic) {
    OnResumed();
  }
}

void MusicIntent::SetPendingList(PendingListType type,
                                 std::vector<std::string> items) {
  pending_list_type_ = type;
  pending_list_ = std::move(items);
}

void MusicIntent::ClearPendingList() {
  pending_list_type_ = PendingListType::kNone;
  pending_list_.clear();
}

bool MusicIntent::StartPendingList() {
  const PendingListType type = pending_list_type_;
  std::vector<std::string> items = std::move(pending_list_);
  ClearPendingList();

  if (type == PendingListType::kKuwoItemIds) {
    return app_->audio_player_.PlayMusicItemList(
        items, [this](const std::string &item_id,
                      AudioPlayer::MusicUrlResolveCallback callback) {
          return QueueKuwoUrlResolve(item_id, std::move(callback));
        });
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
  auto context = std::make_unique<KuwoUrlTaskContext>();
  context->intent = this;
  context->item_id = item_id;
  context->callback = std::move(callback);
  if (xTaskCreate(ResolveKuwoUrlTask, "kuwo_resolve", 8192, context.get(), 4,
                  nullptr) != pdPASS) {
    ESP_LOGE(kTag, "创建酷我 URL 解析任务失败");
    return false;
  }
  context.release();
  return true;
}

void MusicIntent::ResolveKuwoUrlTask(void *arg) {
  std::unique_ptr<KuwoUrlTaskContext> context(
      static_cast<KuwoUrlTaskContext *>(arg));
  auto *intent = context->intent;
  auto *app = intent->app_;
  std::string url;
  bool success = false;
  {
    std::lock_guard<std::mutex> lock(app->xiaoling_conn_mutex_);
    success = app->xiaoling_conn_ != nullptr &&
              app->xiaoling_conn_->ResolveKuwoMusicUrl(context->item_id, url);
  }
  app->Schedule([callback = std::move(context->callback), success,
                 url = std::move(url)]() mutable {
    callback(success, std::move(url));
  });
  vTaskDelete(nullptr);
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
  user_paused_ = false;
  ESP_LOGI(kTag, "音乐事件退出，播放列表已清除");
}
