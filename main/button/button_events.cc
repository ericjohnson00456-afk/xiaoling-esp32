#include "button_events.h"
#include <esp_log.h>

ESP_EVENT_DEFINE_BASE(BUTTON_EVENT);

namespace ButtonEvents {

static_assert(ClassifyClick(1) == kShortPress1);
static_assert(ClassifyClick(2) == kShortPress2);
static_assert(ClassifyClick(3) == kShortPress3);
static_assert(ClassifyClick(4) == kShortPress4);
static_assert(ClassifyClick(5) == kShortPress5To7);
static_assert(ClassifyClick(7) == kShortPress5To7);
static_assert(ClassifyClick(8) == kShortPress8OrMore);
static_assert(ClassifyClick(UINT8_MAX) == kShortPress8OrMore);

esp_err_t Post(Gesture gesture, const Button* source, uint8_t click_count) {
    const Event event{source, click_count};
    const auto result = esp_event_post(BUTTON_EVENT, gesture, &event, sizeof(event), 0);
    if (result != ESP_OK) {
        ESP_LOGW("ButtonEvents", "Failed to post button event %ld: %s",
                 static_cast<long>(gesture), esp_err_to_name(result));
    }
    return result;
}

}
