#include "miniapp_input.h"
#include "miniapp.h"
#include "miniapp_platform.h"
#include <string>
#include <vector>
#include <algorithm>

namespace {
struct Input {
    std::string id, label;
    bool exit_key;
};
std::vector<Input> &Inputs() {
    static std::vector<Input> value;
    return value;
}
} // namespace
void MiniappRegisterButton(const char *id, const char *label, bool exit_key) {
    auto &entries = Inputs();
    auto found = std::find_if(entries.begin(), entries.end(),
                              [id](const Input &input) { return input.id == id; });
    if (found == entries.end())
        entries.push_back({id, label, exit_key});
}
cJSON *MiniappButtonsJson() {
    auto *result = cJSON_CreateArray();
    for (const auto &input : Inputs()) {
        auto *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", input.id.c_str());
        cJSON_AddStringToObject(item, "label", input.label.c_str());
        cJSON_AddItemToObject(item, "events", cJSON_Parse("[\"click\"]"));
        if (input.exit_key)
            cJSON_AddNumberToObject(item, "reserved_hold_ms", 3000);
        cJSON_AddItemToArray(result, item);
    }
    return result;
}
bool MiniappInputActive() {
    return miniapp_is_active() && !miniapp_platform_alarm();
}
extern "C" bool miniapp_button_known(const char *id) {
    for (const auto &input : Inputs())
        if (input.id == id)
            return true;
    return false;
}
