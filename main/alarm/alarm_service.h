#pragma once

#include "player/types/playback_event.h"

#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

enum class AlarmType : uint8_t {
    kOnce = 0,
    kDaily,
    kWeekly,
    kWorkday,
    kWeekend,
    kMonthly,
    kYearly,
    kCustom,
};

struct AlarmSpec {
    std::string id;
    AlarmType type = AlarmType::kOnce;
    std::string calendar = "GREGORIAN";
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = -1;
    int minute = -1;
    int second = 0;
    int day_of_week = 0;
    int day_of_month = 0;
    int month_of_year = 0;
    std::string holiday_name;
    std::string text;
    // 云端指令和列表未提供该字段时关闭稍后提醒，仅显式 true 时开启。
    bool snooze_enabled = false;
    int snooze_interval = 5;
    int snooze_count = 3;
};

struct AlarmSnapshot {
    AlarmSpec spec;
    int64_t next_trigger = 0;
    int snooze_used = 0;
};

class AlarmService {
public:
    static AlarmService& GetInstance();

    bool Initialize();
    void Tick();
    bool Create(const AlarmSpec& spec, std::string& error);
    bool Update(const AlarmSpec& spec, std::string& error);
    bool Delete(const std::string& id, std::string& error);
    bool Find(const std::string& id, AlarmSnapshot& alarm) const;
    std::vector<AlarmSnapshot> GetAll() const;

    bool IsRinging() const;
    // True only while at least one runtime alarm is inside the local timer
    // window. Far-future persisted alarms intentionally do not count.
    bool HasActiveCountdown() const;
    bool CanSnooze() const;
    void Snooze();
    void Stop(bool show_toast = false);
    void HandleTtsUrl(bool success, std::string url);
    void RequestCloudSync();

    static const char* TypeName(AlarmType type);
    static bool ParseType(const std::string& value, AlarmType& type);
    static bool ParseDate(const std::string& value, int& year, int& month, int& day);
    static bool ParseTime(const std::string& value, int& hour, int& minute, int& second);
    static void LocalDateTime(int64_t timestamp, int& year, int& month, int& day,
                              int& hour, int& minute, int& second, int& weekday);

private:
    AlarmService() = default;

    bool BuildAlarm(const AlarmSpec& spec, int64_t now, AlarmSnapshot& alarm,
                    std::string& error) const;
    int64_t CalculateNext(const AlarmSpec& spec, int64_t after) const;
    bool Load();
    bool Save() const;
    void StartRinging(const AlarmSnapshot& alarm, int64_t now);
    void BeginAlarmTts();
    void BeginLoopTone();
    bool SyncFromCloud();
    void FinishRinging(bool advance_schedule);
    void AdvanceOrRemove(const std::string& id, int64_t after);
    void HandlePlaybackEvent(const PlaybackEvent& event);
    int FindIndex(const std::string& id) const;

    std::vector<AlarmSnapshot> alarms_;
    bool initialized_ = false;
    bool ringing_ = false;
    enum class RingPhase : uint8_t {
        kNone,
        kInitialTone,
        kWaitingTtsUrl,
        kTts,
        kLoopTone,
    };
    RingPhase ring_phase_ = RingPhase::kNone;
    bool current_pre_advanced_ = false;
    AlarmSnapshot current_alarm_;
    int64_t ring_started_at_ = 0;
    uint32_t tone_session_id_ = 0;
    int64_t tts_request_deadline_ = 0;
    bool time_reconciled_ = false;
    std::vector<uint32_t> playback_event_subscription_ids_;
    std::atomic_bool cloud_sync_in_progress_{false};
    mutable std::recursive_mutex mutex_;
};
