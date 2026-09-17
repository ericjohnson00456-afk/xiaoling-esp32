#include "button/button.h"
#include "button/button_events.h"

#include <button_gpio.h>
#include <esp_log.h>

#define TAG "Button"

#if CONFIG_SOC_ADC_SUPPORTED
AdcButton::AdcButton(const button_adc_config_t& adc_config) : Button(nullptr) {
    button_config_t btn_config = {
        .long_press_time = 2000,
        .short_press_time = 0,
    };
    ESP_ERROR_CHECK(iot_button_new_adc_device(&btn_config, &adc_config, &button_handle_));
}
#endif

Button::Button(button_handle_t button_handle) : button_handle_(button_handle) {
}

Button::Button(gpio_num_t gpio_num, bool active_high, uint16_t long_press_time, uint16_t short_press_time, bool enable_power_save) : gpio_num_(gpio_num) {
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
}

Button::~Button() {
    if (button_handle_ != NULL) {
        iot_button_delete(button_handle_);
    }
    for (auto* handler : multiple_click_handlers_) {
        delete handler;
    }
}

esp_err_t Button::EnableEventPublishing() {
    if (button_handle_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event_publishing_enabled_) {
        return ESP_OK;
    }
    esp_err_t result = iot_button_register_cb(button_handle_, BUTTON_PRESS_REPEAT_DONE, nullptr,
        [](void* handle, void* user_data) {
            auto* button = static_cast<Button*>(user_data);
            const auto count = iot_button_get_repeat(static_cast<button_handle_t>(handle));
            if (count == 0) {
                ESP_LOGW(TAG, "Ignore completed press sequence with zero clicks");
                return;
            }
            ButtonEvents::Post(ButtonEvents::ClassifyClick(count), button, count);
        }, this);
    if (result != ESP_OK) {
        return result;
    }
    result = iot_button_register_cb(button_handle_, BUTTON_LONG_PRESS_START, nullptr,
        [](void* handle, void* user_data) {
            (void)handle;
            ButtonEvents::Post(ButtonEvents::kLongPress, static_cast<Button*>(user_data));
        }, this);
    if (result != ESP_OK) {
        iot_button_unregister_cb(button_handle_, BUTTON_PRESS_REPEAT_DONE, nullptr);
        return result;
    }
    event_publishing_enabled_ = true;
    return ESP_OK;
}

void Button::OnPressDown(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_press_down_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_PRESS_DOWN, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->on_press_down_) {
            button->on_press_down_();
        }
    }, this);
}

void Button::OnPressUp(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_press_up_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_PRESS_UP, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        if (button->on_press_up_) {
            button->on_press_up_();
        }
    }, this);
}

void Button::OnLongPress(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_long_press_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_LONG_PRESS_START, nullptr, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
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
        if (button->on_double_click_) {
            button->on_double_click_();
        }
    }, this);
}

void Button::OnMultipleClick(std::function<void()> callback, uint8_t click_count) {
    if (button_handle_ == nullptr) {
        return;
    }
    auto* handler = new MultipleClickHandler{std::move(callback)};
    multiple_click_handlers_.push_back(handler);
    button_event_args_t event_args = {
        .multiple_clicks = {
            .clicks = click_count
        }
    };
    iot_button_register_cb(button_handle_, BUTTON_MULTIPLE_CLICK, &event_args, [](void* handle, void* usr_data) {
        (void)handle;
        auto* handler = static_cast<MultipleClickHandler*>(usr_data);
        if (handler->callback) {
            handler->callback();
        }
    }, handler);
}
