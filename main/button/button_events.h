#pragma once

#include <esp_event.h>

class Button;

ESP_EVENT_DECLARE_BASE(BUTTON_EVENT);

namespace ButtonEvents {

enum Gesture : int32_t {
    kShortPress1,
    kShortPress2,
    kShortPress3,
    kShortPress4,
    kShortPress5To7,
    kShortPress8OrMore,
    kLongPress,
};

struct Event {
    const Button* source;
    uint8_t click_count; // 0 for long press
};

constexpr Gesture ClassifyClick(uint8_t count) {
    return count == 1 ? kShortPress1 : count == 2 ? kShortPress2 :
           count == 3 ? kShortPress3 : count == 4 ? kShortPress4 :
           count < 8 ? kShortPress5To7 : kShortPress8OrMore;
}

esp_err_t Post(Gesture gesture, const Button* source, uint8_t click_count = 0);

}
