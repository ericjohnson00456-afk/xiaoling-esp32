#include <stdexcept>

#include "mcp_server.h"

namespace {
MCP_REGISTER_TOOL(RegisterAlarmClock, McpToolGroup::kCommon, 150) {
  server.AddTool(
      "ls.built_in.alarm_clock",
      "智能闹钟与提醒管理系统。支持公历/农历、固定周期及动态节假日。"
      "核心准则：1.【禁止猜测时间】若用户仅提供'下午'、'早晨'、'待会'、'以后'等模糊时段，"
      "或未提及具体时间，严禁私自编造时间点，此时必须放弃调用函数，转而向用户询问具体几点几分。"
      "2. 识别到特定节日必须填充 holiday_name；"
      "3. 必须提供 cloud_id 用于闹钟的唯一标识和后续操作。",
      PropertyList({
          Property("action", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","enum":["CREATE","DELETE","UPDATE","QUERY"],"description":"操作类型：CREATE(新建), DELETE(删除), UPDATE(修改), QUERY(查询)。"})"},
                   true),
          Property("alarm_type", kPropertyTypeJson,
                   JsonSchema{R"({"type":"string","enum":["ONCE","DAILY","WEEKLY","WORKDAY","WEEKEND","MONTHLY","YEARLY","CUSTOM"],"description":"循环策略。节日提醒固定使用 YEARLY。CUSTOM 类型必须提供 date 参数，触发后通过API查询下次时间。"})"},
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
      [](const PropertyList &) -> ReturnValue {
        throw std::runtime_error("ls.built_in.alarm_clock 尚未实现");
      });
}
}
