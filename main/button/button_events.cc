#include "button_events.h"
#include "button.h"

#include <esp_log.h>

ESP_EVENT_DEFINE_BASE(BUTTON_EVENT);

namespace ButtonEvents {

esp_err_t Post(Action action) {
    const auto result = esp_event_post(BUTTON_EVENT, action, nullptr, 0, 0);
    if (result != ESP_OK) {
        ESP_LOGW("ButtonEvents", "Failed to post button event %ld: %s",
                 static_cast<long>(action), esp_err_to_name(result));
    }
    return result;
}

void BindWakeupButton(Button& button, bool double_click_toggles_aec) {
    button.OnClick([]() { Post(kClick); });
    button.OnDoubleClick([double_click_toggles_aec]() {
        Post(double_click_toggles_aec ? kToggleAec : kDoubleClick);
    });
    button.OnMultipleClick([]() { Post(kTripleClick); }, 3);
    // 连按 8 下及以上恢复出厂设置。esp-button 的多击回调按精确次数匹配，
    // 因此逐档注册 8~15 次，超过 15 次的连按不常见。
    for (uint8_t clicks = 8; clicks <= 15; ++clicks) {
        button.OnMultipleClick([]() { Post(kFactoryReset); }, clicks);
    }
}

}
