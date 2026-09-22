#include <cmath>
#include <stdexcept>
#include <string>
#include <cstdio>

#include "alarm/alarm_service.h"
#include "mcp_server.h"

namespace {

class JsonArgument {
public:
  explicit JsonArgument(const PropertyList& properties, const char* name) {
    const auto raw = properties[name].value<std::string>();
    if (!raw.empty()) value_ = cJSON_Parse(raw.c_str());
  }
  ~JsonArgument() { cJSON_Delete(value_); }
  cJSON* get() const { return value_; }

private:
  cJSON* value_ = nullptr;
};

std::string StringArgument(const PropertyList& properties, const char* name,
                           const std::string& fallback = {}) {
  JsonArgument argument(properties, name);
  if (cJSON_IsString(argument.get()) && argument.get()->valuestring != nullptr) {
    return argument.get()->valuestring;
  }
  if (cJSON_IsNumber(argument.get())) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.0f", argument.get()->valuedouble);
    return buffer;
  }
  return fallback;
}

std::string RequiredStringArgument(const PropertyList& properties, const char* name) {
  JsonArgument argument(properties, name);
  if (!cJSON_IsString(argument.get()) || argument.get()->valuestring == nullptr) {
    throw std::runtime_error(std::string(name) + " 必须是字符串");
  }
  return argument.get()->valuestring;
}

std::string RequiredIdArgument(const PropertyList& properties) {
  JsonArgument argument(properties, "id");
  if (cJSON_IsString(argument.get()) && argument.get()->valuestring != nullptr) {
    return argument.get()->valuestring;
  }
  if (cJSON_IsNumber(argument.get())) {
    constexpr double kMaxExactJsonInteger = 9007199254740991.0;  // 2^53 - 1
    const double value = argument.get()->valuedouble;
    if (!std::isfinite(value) || value < 1.0 || value > kMaxExactJsonInteger ||
        std::trunc(value) != value) {
      throw std::runtime_error("数字 id 必须是 1 到 2^53-1 之间的整数");
    }
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    return buffer;
  }
  throw std::runtime_error("id 必须是字符串或安全整数");
}

int IntegerArgument(const PropertyList& properties, const char* name, int fallback) {
  JsonArgument argument(properties, name);
  return cJSON_IsNumber(argument.get()) ? argument.get()->valueint : fallback;
}

bool BooleanArgument(const PropertyList& properties, const char* name, bool fallback) {
  JsonArgument argument(properties, name);
  return cJSON_IsBool(argument.get()) ? cJSON_IsTrue(argument.get()) : fallback;
}

cJSON* AlarmJson(const AlarmSnapshot& alarm) {
  auto* result = cJSON_CreateObject();
  cJSON_AddStringToObject(result, "id", alarm.spec.id.c_str());
  cJSON_AddStringToObject(result, "alarm_type", AlarmService::TypeName(alarm.spec.type));
  cJSON_AddNumberToObject(result, "timestamp", static_cast<double>(alarm.next_trigger));
  cJSON_AddStringToObject(result, "text", alarm.spec.text.c_str());
  cJSON_AddBoolToObject(result, "snooze_enabled", alarm.spec.snooze_enabled);
  cJSON_AddNumberToObject(result, "snooze_interval", alarm.spec.snooze_interval);
  cJSON_AddNumberToObject(result, "snooze_count", alarm.spec.snooze_count);
  return result;
}

MCP_REGISTER_TOOL(RegisterAlarmClock, McpToolGroup::kCommon, 150) {
  server.AddTool(
      "ls.built_in.alarm_clock",
      "智能闹钟与提醒管理系统。当前端侧支持公历单次、每天、每周、每月和每年提醒；"
      "WORKDAY/WEEKEND 暂按周一至周五/周六周日计算，不包含法定节假日和调休。"
      "CUSTOM 类型通过日期云服务查询下一次触发日期；农历和法定节假日日期转换暂未接入。"
      "核心准则：1.【禁止猜测时间】若用户仅提供'下午'、'早晨'、'待会'、'以后'等模糊时段，"
      "或未提及具体时间，严禁私自编造时间点，此时必须放弃调用函数，转而向用户询问具体几点几分。"
      "2. 识别到特定节日必须填充 holiday_name；"
      "3. 必须提供 id，用于闹钟的唯一标识和后续操作；64 位 ID 应使用字符串传递。",
      PropertyList({
          Property("id", kPropertyTypeJson,
                   JsonSchema{R"({"oneOf":[{"type":"string","minLength":1,"maxLength":31},{"type":"integer","minimum":1,"maximum":9007199254740991}],"description":"闹钟唯一 ID。兼容安全整数；超过 2^53-1 的 64 位 ID 必须使用十进制字符串，避免精度丢失。"})"},
                   true),
          Property("action", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","enum":["CREATE","DELETE","UPDATE","QUERY"],"description":"操作类型：CREATE(新建), DELETE(删除), UPDATE(修改), QUERY(查询)。"})"},
                   true),
          Property("alarm_type", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","enum":["ONCE","DAILY","WEEKLY","WORKDAY","WEEKEND","MONTHLY","YEARLY","CUSTOM"],"description":"循环策略。节日提醒固定使用 YEARLY。CUSTOM 类型由日期云服务根据 id 查询下一次触发日期。"})"},
                   false),
          Property("holiday_name", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","description":"节日名称（如'中秋节'、'母亲节'）。若用户提及节日，此项必填。"})"},
                   false),
          Property("calendar_type", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","enum":["GREGORIAN","LUNAR"],"default":"GREGORIAN","description":"历法：GREGORIAN(公历), LUNAR(农历)。"})"},
                   false),
          Property("time", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","description":"24小时制时间 (HH:mm:ss)。严禁推测，未提及则反问用户。"})"},
                   false),
          Property("date", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","description":"具体日期 (YYYY-MM-DD)。仅在 ONCE 模式下使用。"})"},
                   false),
          Property("lunar_date", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","description":"具体日期 (MM-DD)。仅在 calendar_type 为 LUNAR 模式下使用。需要输出农历对应的数字月日格式，如'正月初三'输出'01-03'。"})"},
                   false),
          Property("day_of_week", kPropertyTypeJson,
                   JsonSchema{R"({"type":"integer","minimum":1,"maximum":7,"description":"周几（1-7，1代表周一）。仅在 WEEKLY 模式有效。"})"},
                   false),
          Property("day_of_month", kPropertyTypeJson,
                   JsonSchema{R"({"type":"integer","minimum":1,"maximum":31,"description":"每月几号。仅在 MONTHLY 或无节日的 YEARLY 模式有效。"})"},
                   false),
          Property("month_of_year", kPropertyTypeJson,
                   JsonSchema{R"({"type":"integer","minimum":1,"maximum":12,"description":"每年几月。仅在无节日的 YEARLY 模式有效。"})"},
                   false),
          Property("text", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","description":"提醒的完整文本内容。例如：'给妈妈打电话庆祝生日'。"})"},
                   false),
          Property("snooze_enabled", kPropertyTypeJson,
                   JsonSchema{R"({"type":"boolean","default":true,"description":"是否开启稍后提醒。默认开启。"})"},
                   false),
          Property("snooze_interval", kPropertyTypeJson,
                   JsonSchema{R"({"type":"integer","default":5,"minimum":1,"maximum":30,"description":"稍后提醒的间隔时间（分钟）。默认 5 分钟，范围 1-30。"})"},
                   false),
          Property("snooze_count", kPropertyTypeJson,
                   JsonSchema{R"({"type":"integer","default":3,"minimum":1,"maximum":10,"description":"稍后提醒的最大次数。默认 3 次，范围 1-10。"})"},
                   false)}),
      [](const PropertyList& properties) -> ReturnValue {
        auto& service = AlarmService::GetInstance();
        const std::string id = RequiredIdArgument(properties);
        const std::string action = RequiredStringArgument(properties, "action");
        if (id.empty()) throw std::runtime_error("id 不能为空");

        if (action == "DELETE") {
          std::string error;
          if (!service.Delete(id, error)) throw std::runtime_error(error);
          auto* result = cJSON_CreateObject();
          cJSON_AddStringToObject(result, "id", id.c_str());
          cJSON_AddStringToObject(result, "action", "DELETE");
          cJSON_AddBoolToObject(result, "success", true);
          return result;
        }
        if (action == "QUERY") {
          AlarmSnapshot alarm;
          if (!service.Find(id, alarm)) throw std::runtime_error("未找到闹钟");
          return AlarmJson(alarm);
        }
        if (action != "CREATE" && action != "UPDATE") {
          throw std::runtime_error("无效的 action");
        }

        AlarmSpec spec;
        spec.id = id;
        const std::string type = StringArgument(properties, "alarm_type");
        if (!AlarmService::ParseType(type, spec.type)) {
          throw std::runtime_error("CREATE/UPDATE 必须提供有效 alarm_type");
        }
        spec.calendar = StringArgument(properties, "calendar_type", "GREGORIAN");
        spec.holiday_name = StringArgument(properties, "holiday_name");
        spec.text = StringArgument(properties, "text");
        spec.day_of_week = IntegerArgument(properties, "day_of_week", 0);
        spec.day_of_month = IntegerArgument(properties, "day_of_month", 0);
        spec.month_of_year = IntegerArgument(properties, "month_of_year", 0);
        spec.snooze_enabled = BooleanArgument(properties, "snooze_enabled", true);
        spec.snooze_interval = IntegerArgument(properties, "snooze_interval", 5);
        spec.snooze_count = IntegerArgument(properties, "snooze_count", 3);

        const std::string time = StringArgument(properties, "time");
        if (!AlarmService::ParseTime(time, spec.hour, spec.minute, spec.second)) {
          throw std::runtime_error("CREATE/UPDATE 必须提供有效 time");
        }
        const std::string date = StringArgument(properties, "date");
        if (!date.empty() && !AlarmService::ParseDate(date, spec.year, spec.month, spec.day)) {
          throw std::runtime_error("date 必须是有效的 YYYY-MM-DD");
        }
        if (spec.type == AlarmType::kYearly) {
          if (spec.month_of_year == 0) spec.month_of_year = spec.month;
          if (spec.day_of_month == 0) spec.day_of_month = spec.day;
        }

        std::string error;
        const bool success = action == "CREATE" ? service.Create(spec, error)
                                                  : service.Update(spec, error);
        if (!success) throw std::runtime_error(error);
        AlarmSnapshot alarm;
        service.Find(id, alarm);
        auto* result = AlarmJson(alarm);
        cJSON_AddStringToObject(result, "action", action.c_str());
        cJSON_AddBoolToObject(result, "success", true);
        return result;
      });
}
}
