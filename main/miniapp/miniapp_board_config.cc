#include "miniapp_input.h"
#include "miniapp.h"
#include "boards/common/button.h"
#include "miniapp_board_config.h"

// GPIO facts remain in the upstream board config. Missing definitions and
// disconnected inputs are deliberately absent from the advertised controls.
void MiniappConfigureButton(Button &button, gpio_num_t gpio) {
    if (gpio == GPIO_NUM_NC)
        return;
    struct Key {
        gpio_num_t gpio;
        const char *id;
        const char *label;
        bool primary;
    };
    const Key keys[] = {
#ifdef BOOT_BUTTON_GPIO
        {BOOT_BUTTON_GPIO, "function", "唤醒键", true},
#endif
#ifdef BOOT_BUTTON_PIN
        {BOOT_BUTTON_PIN, "function", "唤醒键", true},
#endif
#ifdef VOLUME_UP_BUTTON_GPIO
        {VOLUME_UP_BUTTON_GPIO, "volume_up", "音量加", false},
#endif
#ifdef VOLUME_DOWN_BUTTON_GPIO
        {VOLUME_DOWN_BUTTON_GPIO, "volume_down", "音量减", false},
#endif
#ifdef TOUCH_BUTTON_GPIO
        {TOUCH_BUTTON_GPIO, "touch", "触摸键", true},
#endif
#ifdef ASR_BUTTON_GPIO
        {ASR_BUTTON_GPIO, "asr", "语音键", true},
#endif
#ifdef MAIN_BUTTON_GPIO
        {MAIN_BUTTON_GPIO, "main", "主键", true},
#endif
#ifdef PWR_BUTTON_GPIO
        {PWR_BUTTON_GPIO, "power", "电源键", true},
#endif
#ifdef POWER_BUTTON_GPIO
        {POWER_BUTTON_GPIO, "power", "电源键", true},
#endif
#ifdef PWR_KEY_GPIO
        {PWR_KEY_GPIO, "power", "电源键", true},
#endif
#ifdef KEY_BUTTON_GPIO
        {KEY_BUTTON_GPIO, "key", "按键", true},
#endif
#ifdef USER_BUTTON_GPIO
        {USER_BUTTON_GPIO, "user", "用户键", true},
#endif
#ifdef KEY1_BUTTON_GPIO
        {KEY1_BUTTON_GPIO, "k1", "K1", true},
#endif
#ifdef LEFT_BUTTON_GPIO
        {LEFT_BUTTON_GPIO, "left", "左键", false},
#endif
#ifdef RIGHT_BUTTON_GPIO
        {RIGHT_BUTTON_GPIO, "right", "右键", false},
#endif
#ifdef L_BUTTON_GPIO
        {L_BUTTON_GPIO, "left", "左键", false},
#endif
#ifdef R_BUTTON_GPIO
        {R_BUTTON_GPIO, "right", "右键", false},
#endif
#ifdef M_BUTTON_GPIO
        {M_BUTTON_GPIO, "middle", "中键", false},
#endif
#ifdef WIFI_BUTTON_GPIO
        {WIFI_BUTTON_GPIO, "wifi", "配网键", false},
#endif
#ifdef CMD_BUTTON_GPIO
        {CMD_BUTTON_GPIO, "command", "控制键", false},
#endif
#ifdef AUDIO_WAKE_BUTTON_GPIO
        {AUDIO_WAKE_BUTTON_GPIO, "audio_wake", "语音唤醒", true},
#endif
#ifdef MOVE_WAKE_BUTTON_GPIO
        {MOVE_WAKE_BUTTON_GPIO, "move_wake", "动作唤醒", false},
#endif
        {GPIO_NUM_NC, nullptr, nullptr, false},
    };
    bool has_boot = false;
#ifdef BOOT_BUTTON_GPIO
    has_boot = BOOT_BUTTON_GPIO != GPIO_NUM_NC;
#endif
#ifdef BOOT_BUTTON_PIN
    has_boot = has_boot || BOOT_BUTTON_PIN != GPIO_NUM_NC;
#endif
    for (const auto &key : keys) {
        if (gpio != key.gpio)
            continue;
        const bool exit_key = !strcmp(key.id, "function") ||
                              (!has_boot && key.primary && !miniapp_button_known("function"));
        button.SetMiniapp(exit_key ? "function" : key.id, key.label, exit_key);
        return;
    }
}
