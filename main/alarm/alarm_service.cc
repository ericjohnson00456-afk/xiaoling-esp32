#include "alarm_service.h"

#include "application.h"
#include "system/time_sync.h"
#include "ui/ui_app.h"
#include "ui/screens/alarm/alarm_ring_presenter.h"
#include "ui/screens/alarm/alarm_success_presenter.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

#include <esp_log.h>
#include <nvs.h>

namespace {

constexpr char kTag[] = "AlarmService";
constexpr char kNamespace[] = "alarm";
constexpr char kRecordsKey[] = "records";
constexpr uint32_t kStoreMagic = 0x414C524DU;  // ALRM
constexpr uint16_t kStoreVersion = 1;
constexpr size_t kMaxAlarms = 20;
constexpr size_t kMaxIdLength = 31;
constexpr size_t kMaxTextLength = 384;
constexpr int64_t kChinaOffsetSeconds = 8 * 60 * 60;
constexpr int64_t kRingTimeoutSeconds = 15 * 60;
// Match the source implementation's maximum one-shot software timer period.
// Longer alarms remain persisted and become active when they enter this window.
constexpr int64_t kCountdownWindowSeconds = 24 * 60 * 60;
constexpr uint16_t kAlarmToneId = 94;

#pragma pack(push, 1)
struct StoredAlarm {
    char id[32];
    uint8_t type;
    uint8_t snooze_enabled;
    uint8_t snooze_interval;
    uint8_t snooze_count;
    uint8_t snooze_used;
    int16_t year;
    int8_t month;
    int8_t day;
    int8_t hour;
    int8_t minute;
    int8_t second;
    int8_t day_of_week;
    int8_t day_of_month;
    int8_t month_of_year;
    int64_t next_trigger;
    uint16_t text_length;
};

struct StoreHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t checksum;
};
#pragma pack(pop)

uint32_t Hash(const uint8_t* data, size_t size) {
    uint32_t value = 2166136261U;
    for (size_t i = 0; i < size; ++i) {
        value = (value ^ data[i]) * 16777619U;
    }
    return value;
}

int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

int DaysInMonth(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return kDays[month - 1];
}

int64_t CivilToTimestamp(int year, int month, int day, int hour, int minute, int second) {
    if (year < 1970 || month < 1 || month > 12 || day < 1 ||
        day > DaysInMonth(year, month) || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return 0;
    }
    return DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
           hour * 3600 + minute * 60 + second - kChinaOffsetSeconds;
}

void CopyString(char* destination, size_t capacity, const std::string& source) {
    if (capacity == 0) return;
    std::snprintf(destination, capacity, "%s", source.c_str());
}

bool IsTerminal(const PlaybackEvent& event) {
    return event.state == PlaybackState::kCompleted ||
           event.state == PlaybackState::kStopped ||
           event.state == PlaybackState::kFailed;
}

}  // namespace

AlarmService& AlarmService::GetInstance() {
    static AlarmService instance;
    return instance;
}

bool AlarmService::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (initialized_) return true;
    initialized_ = true;
    const bool loaded = Load();
    time_reconciled_ = TimeSync::IsSystemTimeValid();
    ESP_LOGI(kTag, "闹钟服务已初始化，恢复 %u 个闹钟", static_cast<unsigned>(alarms_.size()));
    return loaded;
}

int AlarmService::FindIndex(const std::string& id) const {
    for (size_t i = 0; i < alarms_.size(); ++i) {
        if (alarms_[i].spec.id == id) return static_cast<int>(i);
    }
    return -1;
}

bool AlarmService::Create(const AlarmSpec& spec, std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_) Initialize();
    if (FindIndex(spec.id) >= 0) {
        error = "闹钟 id 已存在";
        return false;
    }
    if (alarms_.size() >= kMaxAlarms) {
        error = "闹钟数量已达上限";
        return false;
    }
    AlarmSnapshot alarm;
    if (!BuildAlarm(spec, std::time(nullptr), alarm, error)) return false;
    alarms_.push_back(alarm);
    if (!Save()) {
        alarms_.pop_back();
        error = "保存闹钟失败";
        return false;
    }
    UiApp::SetAlarmActive(HasActiveCountdown());
    AlarmSuccessPresenter::GetInstance().ShowAlarm(alarm, true);
    return true;
}

bool AlarmService::Update(const AlarmSpec& spec, std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_) Initialize();
    const int index = FindIndex(spec.id);
    if (index < 0) {
        error = "未找到要修改的闹钟";
        return false;
    }
    AlarmSnapshot replacement;
    if (!BuildAlarm(spec, std::time(nullptr), replacement, error)) return false;
    if (ringing_ && current_alarm_.spec.id == spec.id) FinishRinging(false);
    const AlarmSnapshot previous = alarms_[index];
    alarms_[index] = replacement;
    if (!Save()) {
        alarms_[index] = previous;
        error = "保存闹钟失败";
        return false;
    }
    UiApp::SetAlarmActive(HasActiveCountdown());
    AlarmSuccessPresenter::GetInstance().ShowAlarm(replacement);
    return true;
}

bool AlarmService::Delete(const std::string& id, std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_) Initialize();
    const int index = FindIndex(id);
    if (index < 0) {
        error = "未找到要删除的闹钟";
        return false;
    }
    if (ringing_ && current_alarm_.spec.id == id) FinishRinging(false);
    const AlarmSnapshot previous = alarms_[index];
    alarms_.erase(alarms_.begin() + index);
    if (!Save()) {
        alarms_.insert(alarms_.begin() + index, previous);
        error = "删除闹钟保存失败";
        return false;
    }
    UiApp::SetAlarmActive(HasActiveCountdown());
    return true;
}

bool AlarmService::Find(const std::string& id, AlarmSnapshot& alarm) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const int index = FindIndex(id);
    if (index < 0) return false;
    alarm = alarms_[index];
    return true;
}

std::vector<AlarmSnapshot> AlarmService::GetAll() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return alarms_;
}

bool AlarmService::BuildAlarm(const AlarmSpec& spec, int64_t now, AlarmSnapshot& alarm,
                              std::string& error) const {
    if (spec.id.empty() || spec.id.size() > kMaxIdLength) {
        error = "id 必须是 1-31 字节的字符串";
        return false;
    }
    if (spec.text.size() > kMaxTextLength) {
        error = "提醒文本不能超过 384 字节";
        return false;
    }
    if (spec.calendar != "GREGORIAN") {
        error = "当前固件尚未接入农历转换服务";
        return false;
    }
    if (spec.type == AlarmType::kCustom) {
        error = "当前固件尚未接入 CUSTOM 下一日期服务";
        return false;
    }
    if (!spec.holiday_name.empty()) {
        error = "当前固件尚未接入法定节假日日期服务";
        return false;
    }
    if (!TimeSync::IsSystemTimeValid()) {
        error = "系统时间尚未校准，请稍后再试";
        return false;
    }
    if (spec.hour < 0 || spec.hour > 23 || spec.minute < 0 || spec.minute > 59 ||
        spec.second < 0 || spec.second > 59) {
        error = "time 必须是有效的 HH:mm 或 HH:mm:ss";
        return false;
    }
    if (spec.snooze_interval < 1 || spec.snooze_interval > 30 ||
        spec.snooze_count < 1 || spec.snooze_count > 10) {
        error = "稍后提醒间隔或次数超出范围";
        return false;
    }
    if (spec.type == AlarmType::kWeekly && (spec.day_of_week < 1 || spec.day_of_week > 7)) {
        error = "WEEKLY 必须提供 1-7 的 day_of_week";
        return false;
    }
    if (spec.type == AlarmType::kMonthly &&
        (spec.day_of_month < 1 || spec.day_of_month > 31)) {
        error = "MONTHLY 必须提供 1-31 的 day_of_month";
        return false;
    }
    if (spec.type == AlarmType::kYearly &&
        ((spec.month_of_year < 1 || spec.month_of_year > 12) ||
         (spec.day_of_month < 1 || spec.day_of_month > 31))) {
        error = "YEARLY 必须提供 month_of_year 和 day_of_month";
        return false;
    }
    AlarmSpec normalized = spec;
    if (normalized.type == AlarmType::kOnce && normalized.year == 0) {
        int h = 0, m = 0, s = 0, weekday = 0;
        LocalDateTime(now, normalized.year, normalized.month, normalized.day, h, m, s, weekday);
    }
    alarm.spec = normalized;
    alarm.next_trigger = CalculateNext(normalized, now);
    alarm.snooze_used = 0;
    if (alarm.next_trigger <= now) {
        error = normalized.type == AlarmType::kOnce ? "闹钟时间已经过去" : "无法计算下次响铃时间";
        return false;
    }
    return true;
}

int64_t AlarmService::CalculateNext(const AlarmSpec& spec, int64_t after) const {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, weekday = 0;
    LocalDateTime(after, year, month, day, hour, minute, second, weekday);
    if (spec.type == AlarmType::kOnce) {
        return CivilToTimestamp(spec.year, spec.month, spec.day, spec.hour, spec.minute, spec.second);
    }
    if (spec.type == AlarmType::kDaily) {
        int64_t candidate = CivilToTimestamp(year, month, day, spec.hour, spec.minute, spec.second);
        return candidate > after ? candidate : candidate + 86400;
    }
    if (spec.type == AlarmType::kWeekly) {
        const int current_monday_based = weekday == 0 ? 7 : weekday;
        int delta = (spec.day_of_week - current_monday_based + 7) % 7;
        int64_t candidate = CivilToTimestamp(year, month, day, spec.hour, spec.minute, spec.second) +
                            static_cast<int64_t>(delta) * 86400;
        return candidate > after ? candidate : candidate + 7 * 86400;
    }
    if (spec.type == AlarmType::kWorkday || spec.type == AlarmType::kWeekend) {
        int64_t base = CivilToTimestamp(year, month, day, spec.hour, spec.minute, spec.second);
        for (int offset = 0; offset < 8; ++offset) {
            const int64_t candidate = base + static_cast<int64_t>(offset) * 86400;
            int cy, cm, cd, ch, cmin, cs, cw;
            LocalDateTime(candidate, cy, cm, cd, ch, cmin, cs, cw);
            const bool workday = cw >= 1 && cw <= 5;
            if (candidate > after && (workday == (spec.type == AlarmType::kWorkday))) return candidate;
        }
        return 0;
    }
    if (spec.type == AlarmType::kMonthly) {
        int candidate_year = year;
        int candidate_month = month;
        for (int i = 0; i < 24; ++i) {
            const int candidate_day = std::min(spec.day_of_month,
                                               DaysInMonth(candidate_year, candidate_month));
            const int64_t candidate = CivilToTimestamp(candidate_year, candidate_month, candidate_day,
                                                       spec.hour, spec.minute, spec.second);
            if (candidate > after) return candidate;
            if (++candidate_month > 12) {
                candidate_month = 1;
                ++candidate_year;
            }
        }
        return 0;
    }
    if (spec.type == AlarmType::kYearly) {
        for (int candidate_year = year; candidate_year <= year + 8; ++candidate_year) {
            const int candidate_day = std::min(spec.day_of_month,
                                               DaysInMonth(candidate_year, spec.month_of_year));
            const int64_t candidate = CivilToTimestamp(candidate_year, spec.month_of_year, candidate_day,
                                                       spec.hour, spec.minute, spec.second);
            if (candidate > after) return candidate;
        }
    }
    return 0;
}

void AlarmService::Tick() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_) Initialize();
    const int64_t now = std::time(nullptr);
    if (!TimeSync::IsSystemTimeValid()) return;
    if (!time_reconciled_) {
        bool changed = false;
        for (auto it = alarms_.begin(); it != alarms_.end();) {
            if (it->next_trigger > now) {
                ++it;
                continue;
            }
            changed = true;
            if (it->spec.type == AlarmType::kOnce) {
                it = alarms_.erase(it);
                continue;
            }
            it->next_trigger = CalculateNext(it->spec, now);
            it->snooze_used = 0;
            if (it->next_trigger <= now) it = alarms_.erase(it);
            else ++it;
        }
        time_reconciled_ = true;
        if (changed) {
            Save();
            UiApp::SetAlarmActive(HasActiveCountdown());
        }
    }
    if (ringing_) {
        if (now - ring_started_at_ >= kRingTimeoutSeconds) Stop();
        else if (tone_session_id_ == 0) tone_session_id_ = Application::GetInstance().StartAlarmTone(kAlarmToneId);
        return;
    }
    auto due = alarms_.end();
    for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
        if (it->next_trigger <= now && (due == alarms_.end() || it->next_trigger < due->next_trigger ||
            (it->next_trigger == due->next_trigger && it->spec.id < due->spec.id))) {
            due = it;
        }
    }
    if (due != alarms_.end()) StartRinging(*due, now);
}

void AlarmService::StartRinging(const AlarmSnapshot& alarm, int64_t now) {
    current_alarm_ = alarm;
    ringing_ = true;
    current_pre_advanced_ = !alarm.spec.snooze_enabled;
    ring_started_at_ = now;
    tone_session_id_ = Application::GetInstance().StartAlarmTone(kAlarmToneId);
    if (current_pre_advanced_) AdvanceOrRemove(alarm.spec.id, now);
    AlarmRingPresenter::GetInstance().ShowAlarm(current_alarm_);
    ESP_LOGI(kTag, "闹钟响铃: id=%s, text=%s", alarm.spec.id.c_str(), alarm.spec.text.c_str());
}

bool AlarmService::IsRinging() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ringing_;
}

bool AlarmService::HasActiveCountdown() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_ || !TimeSync::IsSystemTimeValid()) return false;

    const int64_t now = std::time(nullptr);
    return std::any_of(alarms_.begin(), alarms_.end(), [now](const AlarmSnapshot& alarm) {
        const int64_t remaining = alarm.next_trigger - now;
        return remaining > 0 && remaining <= kCountdownWindowSeconds;
    });
}

bool AlarmService::CanSnooze() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ringing_ && current_alarm_.spec.snooze_enabled &&
           current_alarm_.snooze_used < current_alarm_.spec.snooze_count;
}

void AlarmService::Snooze() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!CanSnooze()) {
        Stop();
        return;
    }
    const int index = FindIndex(current_alarm_.spec.id);
    if (index >= 0) {
        alarms_[index].snooze_used = current_alarm_.snooze_used + 1;
        alarms_[index].next_trigger = std::time(nullptr) +
                                      current_alarm_.spec.snooze_interval * 60;
        Save();
        UiApp::SetAlarmActive(HasActiveCountdown());
    }
    FinishRinging(false);
    UiApp::ShowToast("稍后提醒您~");
}

void AlarmService::Stop(bool show_toast) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ringing_) return;
    const bool recurring = current_alarm_.spec.type != AlarmType::kOnce;
    FinishRinging(!current_pre_advanced_);
    if (show_toast) {
        UiApp::ShowToast(recurring ? "本次闹钟已关闭" : "闹钟关闭成功!");
    }
}

void AlarmService::FinishRinging(bool advance_schedule) {
    const std::string id = current_alarm_.spec.id;
    Application::GetInstance().StopAlarmTone();
    ringing_ = false;
    tone_session_id_ = 0;
    if (advance_schedule) AdvanceOrRemove(id, std::time(nullptr));
    current_pre_advanced_ = false;
    AlarmRingPresenter::GetInstance().Dismiss();
}

void AlarmService::AdvanceOrRemove(const std::string& id, int64_t after) {
    const int index = FindIndex(id);
    if (index < 0) return;
    if (alarms_[index].spec.type == AlarmType::kOnce) {
        alarms_.erase(alarms_.begin() + index);
    } else {
        alarms_[index].next_trigger = CalculateNext(alarms_[index].spec, after);
        alarms_[index].snooze_used = 0;
        if (alarms_[index].next_trigger <= after) alarms_.erase(alarms_.begin() + index);
    }
    Save();
    UiApp::SetAlarmActive(HasActiveCountdown());
}

void AlarmService::HandlePlaybackEvent(const PlaybackEvent& event) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ringing_ || event.session_id != tone_session_id_ || !IsTerminal(event)) return;
    tone_session_id_ = 0;
    if (event.state == PlaybackState::kCompleted || event.state == PlaybackState::kFailed) {
        tone_session_id_ = Application::GetInstance().StartAlarmTone(kAlarmToneId);
    }
}

bool AlarmService::Load() {
    alarms_.clear();
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    size_t size = 0;
    result = nvs_get_blob(handle, kRecordsKey, nullptr, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return true;
    }
    if (result != ESP_OK || size < sizeof(StoreHeader)) {
        nvs_close(handle);
        return false;
    }
    std::vector<uint8_t> blob(size);
    result = nvs_get_blob(handle, kRecordsKey, blob.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) return false;
    StoreHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    const size_t payload_size = blob.size() - sizeof(header);
    if (header.magic != kStoreMagic || header.version != kStoreVersion ||
        header.count > kMaxAlarms ||
        Hash(blob.data() + sizeof(header), payload_size) != header.checksum) {
        ESP_LOGE(kTag, "闹钟持久化数据校验失败，忽略旧数据");
        return false;
    }
    const int64_t now = std::time(nullptr);
    size_t offset = sizeof(header);
    bool changed = false;
    for (size_t i = 0; i < header.count; ++i) {
        if (offset + sizeof(StoredAlarm) > blob.size()) {
            alarms_.clear();
            return false;
        }
        StoredAlarm record{};
        std::memcpy(&record, blob.data() + offset, sizeof(record));
        offset += sizeof(record);
        if (record.id[sizeof(record.id) - 1] != '\0' ||
            record.type > static_cast<uint8_t>(AlarmType::kCustom) ||
            record.text_length > kMaxTextLength || offset + record.text_length > blob.size()) {
            alarms_.clear();
            return false;
        }
        AlarmSnapshot alarm;
        alarm.spec.id = record.id;
        alarm.spec.type = static_cast<AlarmType>(record.type);
        alarm.spec.year = record.year;
        alarm.spec.month = record.month;
        alarm.spec.day = record.day;
        alarm.spec.hour = record.hour;
        alarm.spec.minute = record.minute;
        alarm.spec.second = record.second;
        alarm.spec.day_of_week = record.day_of_week;
        alarm.spec.day_of_month = record.day_of_month;
        alarm.spec.month_of_year = record.month_of_year;
        alarm.spec.text.assign(reinterpret_cast<const char*>(blob.data() + offset), record.text_length);
        offset += record.text_length;
        alarm.spec.snooze_enabled = record.snooze_enabled != 0;
        alarm.spec.snooze_interval = record.snooze_interval;
        alarm.spec.snooze_count = record.snooze_count;
        alarm.snooze_used = record.snooze_used;
        alarm.next_trigger = record.next_trigger;
        if (TimeSync::IsSystemTimeValid() && alarm.next_trigger <= now) {
            if (alarm.spec.type == AlarmType::kOnce) {
                changed = true;
                continue;
            }
            alarm.next_trigger = CalculateNext(alarm.spec, now);
            alarm.snooze_used = 0;
            changed = true;
        }
        if (alarm.next_trigger > 0) alarms_.push_back(std::move(alarm));
    }
    if (offset != blob.size()) {
        alarms_.clear();
        return false;
    }
    if (changed) Save();
    return true;
}

bool AlarmService::Save() const {
    StoreHeader header{kStoreMagic, kStoreVersion, static_cast<uint16_t>(alarms_.size()), 0};
    std::vector<uint8_t> payload;
    for (const auto& alarm : alarms_) {
        StoredAlarm record{};
        CopyString(record.id, sizeof(record.id), alarm.spec.id);
        record.type = static_cast<uint8_t>(alarm.spec.type);
        record.snooze_enabled = alarm.spec.snooze_enabled ? 1 : 0;
        record.snooze_interval = static_cast<uint8_t>(alarm.spec.snooze_interval);
        record.snooze_count = static_cast<uint8_t>(alarm.spec.snooze_count);
        record.snooze_used = static_cast<uint8_t>(alarm.snooze_used);
        record.year = alarm.spec.year;
        record.month = alarm.spec.month;
        record.day = alarm.spec.day;
        record.hour = alarm.spec.hour;
        record.minute = alarm.spec.minute;
        record.second = alarm.spec.second;
        record.day_of_week = alarm.spec.day_of_week;
        record.day_of_month = alarm.spec.day_of_month;
        record.month_of_year = alarm.spec.month_of_year;
        record.next_trigger = alarm.next_trigger;
        record.text_length = static_cast<uint16_t>(alarm.spec.text.size());
        const auto* record_bytes = reinterpret_cast<const uint8_t*>(&record);
        payload.insert(payload.end(), record_bytes, record_bytes + sizeof(record));
        payload.insert(payload.end(), alarm.spec.text.begin(), alarm.spec.text.end());
    }
    const size_t payload_size = payload.size();
    header.checksum = Hash(payload.data(), payload_size);
    std::vector<uint8_t> blob(sizeof(header) + payload_size);
    std::memcpy(blob.data(), &header, sizeof(header));
    if (payload_size != 0) std::memcpy(blob.data() + sizeof(header), payload.data(), payload_size);
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_blob(handle, kRecordsKey, blob.data(), blob.size());
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    if (result != ESP_OK) ESP_LOGE(kTag, "保存闹钟失败: %s", esp_err_to_name(result));
    return result == ESP_OK;
}

const char* AlarmService::TypeName(AlarmType type) {
    static constexpr std::array<const char*, 8> kNames = {
        "ONCE", "DAILY", "WEEKLY", "WORKDAY", "WEEKEND", "MONTHLY", "YEARLY", "CUSTOM"};
    const size_t index = static_cast<size_t>(type);
    return index < kNames.size() ? kNames[index] : "UNKNOWN";
}

bool AlarmService::ParseType(const std::string& value, AlarmType& type) {
    for (size_t i = 0; i <= static_cast<size_t>(AlarmType::kCustom); ++i) {
        if (value == TypeName(static_cast<AlarmType>(i))) {
            type = static_cast<AlarmType>(i);
            return true;
        }
    }
    return false;
}

bool AlarmService::ParseDate(const std::string& value, int& year, int& month, int& day) {
    char tail = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%d%c", &year, &month, &day, &tail) != 3) return false;
    return year >= 1970 && year <= 9999 &&
           CivilToTimestamp(year, month, day, 0, 0, 0) != 0;
}

bool AlarmService::ParseTime(const std::string& value, int& hour, int& minute, int& second) {
    int consumed = 0;
    second = 0;
    if (std::sscanf(value.c_str(), "%d:%d%n", &hour, &minute, &consumed) != 2) return false;
    if (consumed < 0 || static_cast<size_t>(consumed) > value.size()) return false;
    if (static_cast<size_t>(consumed) != value.size()) {
        if (value[consumed] != ':') return false;
        int seconds_consumed = 0;
        if (std::sscanf(value.c_str() + consumed + 1, "%d%n", &second, &seconds_consumed) != 1 ||
            consumed + 1 + seconds_consumed != static_cast<int>(value.size())) {
            return false;
        }
    }
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
}

void AlarmService::LocalDateTime(int64_t timestamp, int& year, int& month, int& day,
                                 int& hour, int& minute, int& second, int& weekday) {
    const std::time_t local_epoch = static_cast<std::time_t>(timestamp + kChinaOffsetSeconds);
    std::tm value{};
    gmtime_r(&local_epoch, &value);
    year = value.tm_year + 1900;
    month = value.tm_mon + 1;
    day = value.tm_mday;
    hour = value.tm_hour;
    minute = value.tm_min;
    second = value.tm_sec;
    weekday = value.tm_wday;
}
