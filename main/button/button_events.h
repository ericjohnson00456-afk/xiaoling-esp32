#pragma once

#include <esp_event.h>

class Button;

ESP_EVENT_DECLARE_BASE(BUTTON_EVENT);

namespace ButtonEvents {

enum Action : int32_t {
    kClick,
    kDoubleClick,
    kPressDown,
    kPressUp,
    kToggleAec,
};

esp_err_t Post(Action action);
void BindWakeupButton(Button& button, bool double_click_toggles_aec = false);

}
