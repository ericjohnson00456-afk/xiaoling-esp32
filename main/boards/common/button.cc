#include "miniapp/miniapp.h"
#include "miniapp/miniapp_input.h"
#include "miniapp/miniapp_platform.h"
#include "button.h"

#include <button_gpio.h>
#include <esp_log.h>
#include <utility>

#define TAG "Button"

#if CONFIG_SOC_ADC_SUPPORTED
AdcButton::AdcButton(const button_adc_config_t& adc_config) : Button(nullptr) {
    button_config_t btn_config = {
        .long_press_time = 2000,
        .short_press_time = 0,
    };
    ESP_ERROR_CHECK(iot_button_new_adc_device(&btn_config, &adc_config, &button_handle_));
    InitializeMiniappInput();
    MiniappConfigureButton(*this, gpio_num_);
}
#endif

Button::Button(button_handle_t button_handle) : button_handle_(button_handle) {
    InitializeMiniappInput();
    MiniappConfigureButton(*this, gpio_num_);
}

Button::Button(gpio_num_t gpio_num, bool active_high, uint16_t long_press_time,
               uint16_t short_press_time, bool enable_power_save)
    : gpio_num_(gpio_num) {
    if (gpio_num == GPIO_NUM_NC) {
        return;
    }
    button_config_t button_config = {
        .long_press_time = long_press_time,
        .short_press_time = short_press_time
    };
    button_gpio_config_t gpio_config = {
        .gpio_num = gpio_num,
        .active_level = static_cast<uint8_t>(active_high ? 1 : 0),
        .enable_power_save = enable_power_save,
        .disable_pull = false
    };
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&button_config, &gpio_config, &button_handle_));
    InitializeMiniappInput();
    MiniappConfigureButton(*this, gpio_num_);
}

Button::~Button() {
    if (miniapp_hold_timer_) { esp_timer_stop(miniapp_hold_timer_); esp_timer_delete(miniapp_hold_timer_); }
    if (button_handle_ != nullptr) {
        iot_button_delete(button_handle_);
    }
}

void Button::OnPressDown(std::function<void()> callback) { on_press_down_ = std::move(callback); }
void Button::OnPressUp(std::function<void()> callback) { on_press_up_ = std::move(callback); }

void Button::SetMiniapp(const char* id, const char* label, bool exit_key) {
    if (!button_handle_) return;
    miniapp_id_ = id;
    miniapp_exit_key_ = exit_key;
    MiniappRegisterButton(id, label, exit_key);
}
bool Button::SuppressForMiniapp() const {
    return !miniapp_id_.empty() && (MiniappInputActive() || miniapp_pressed_ ||
                                   esp_timer_get_time() < miniapp_suppress_until_);
}
void Button::InitializeMiniappInput() {
    if (!button_handle_) return;

#if CONFIG_MINIAPP
    const esp_timer_create_args_t args = {
        .callback = [](void* value) {
            auto* button = static_cast<Button*>(value);
#if CONFIG_MINIAPP
            if (button->miniapp_pressed_ && MiniappInputActive()) {
                button->miniapp_held_ = true;
                miniapp_exit();
            }
#endif
        }, .arg = this, .dispatch_method = ESP_TIMER_TASK, .name = "miniapp_hold",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &miniapp_hold_timer_));
#endif
    iot_button_register_cb(button_handle_, BUTTON_PRESS_DOWN, nullptr, [](void*, void* value) {
        auto* button = static_cast<Button*>(value);
        button->miniapp_pressed_ = !button->miniapp_id_.empty() && MiniappInputActive();
        button->miniapp_held_ = false;
        if (button->miniapp_pressed_) {
            if (button->miniapp_exit_key_) esp_timer_start_once(button->miniapp_hold_timer_, 3000000);
        } else if (button->on_press_down_) button->on_press_down_();
    }, this);
    iot_button_register_cb(button_handle_, BUTTON_PRESS_UP, nullptr, [](void*, void* value) {
        auto* button = static_cast<Button*>(value);
        if (button->miniapp_hold_timer_) esp_timer_stop(button->miniapp_hold_timer_);
        if (button->miniapp_pressed_) {
#if CONFIG_MINIAPP
            if (!button->miniapp_held_ && MiniappInputActive()) miniapp_button_click(button->miniapp_id_.c_str());
#endif
            button->miniapp_pressed_ = false;
            button->miniapp_suppress_until_ = esp_timer_get_time() + 1000000;
        } else if (button->on_press_up_) button->on_press_up_();
    }, this);
}

void Button::OnLongPress(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_long_press_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_LONG_PRESS_START, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->SuppressForMiniapp()) return;
        if (button->on_long_press_) {
            button->on_long_press_();
        }
    }, this);
}

void Button::OnClick(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_click_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_SINGLE_CLICK, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->SuppressForMiniapp()) return;
        if (button->on_click_) {
            button->on_click_();
        }
    }, this);
}

void Button::OnDoubleClick(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_double_click_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_DOUBLE_CLICK, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->SuppressForMiniapp()) return;
        if (button->on_double_click_) {
            button->on_double_click_();
        }
    }, this);
}

void Button::OnMultipleClick(std::function<void()> callback, uint8_t click_count) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_multiple_click_[click_count] = std::move(callback);
    button_event_args_t event_args = {
        .multiple_clicks = {
            .clicks = click_count
        }
    };
    iot_button_register_cb(button_handle_, BUTTON_MULTIPLE_CLICK, &event_args, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->SuppressForMiniapp()) return;
        const auto click_count = iot_button_get_repeat(static_cast<button_handle_t>(handle));
        const auto callback = button->on_multiple_click_.find(click_count);
        if (callback != button->on_multiple_click_.end() && callback->second) {
            callback->second();
        }
    }, this);
}

void Button::OnMultipleClickAtLeast(std::function<void(uint8_t)> callback, uint8_t min_click_count) {
    if (button_handle_ == nullptr || min_click_count == 0) {
        return;
    }
    on_multiple_click_at_least_ = std::move(callback);
    min_multiple_click_count_ = min_click_count;
    iot_button_register_cb(button_handle_, BUTTON_PRESS_REPEAT_DONE, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->SuppressForMiniapp()) return;
        const auto click_count = iot_button_get_repeat(static_cast<button_handle_t>(handle));
        if (click_count >= button->min_multiple_click_count_ && button->on_multiple_click_at_least_) {
            button->on_multiple_click_at_least_(click_count);
        }
    }, this);
}
