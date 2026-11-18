#pragma once
#include "sdkconfig.h"
#include <driver/gpio.h>
class Button;
#if CONFIG_MINIAPP
#include <cJSON.h>
void MiniappConfigureButton(Button &button, gpio_num_t gpio);
void MiniappRegisterButton(const char *id, const char *label, bool exit_key);
cJSON *MiniappButtonsJson();
bool MiniappInputActive();
#else
inline void MiniappConfigureButton(Button &, gpio_num_t) {
}
inline void MiniappRegisterButton(const char *, const char *, bool) {
}
inline bool MiniappInputActive() {
    return false;
}
#endif
