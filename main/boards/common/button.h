#ifndef BUTTON_H_
#define BUTTON_H_

#include <driver/gpio.h>
#include <iot_button.h>
#include <button_types.h>
#include <button_adc.h>
#include <button_gpio.h>
#include <functional>
#include <string>
#include <esp_timer.h>
#include <unordered_map>

class Button {
public:
    Button(button_handle_t button_handle);
    Button(gpio_num_t gpio_num, bool active_high = false, uint16_t long_press_time = 0,
           uint16_t short_press_time = 0, bool enable_power_save = false);
    ~Button();

    void SetMiniapp(const char* id, const char* label, bool exit_key = false);

    void OnPressDown(std::function<void()> callback);
    void OnPressUp(std::function<void()> callback);
    void OnLongPress(std::function<void()> callback);
    void OnClick(std::function<void()> callback);
    void OnDoubleClick(std::function<void()> callback);
    void OnMultipleClick(std::function<void()> callback, uint8_t click_count = 3);
    // Invoke the callback when the click count reaches the threshold.
    void OnMultipleClickAtLeast(std::function<void(uint8_t)> callback, uint8_t min_click_count);

protected:
    void InitializeMiniappInput();
    bool SuppressForMiniapp() const;
    std::string miniapp_id_;
    bool miniapp_exit_key_ = false;
    bool miniapp_pressed_ = false;
    bool miniapp_held_ = false;
    int64_t miniapp_suppress_until_ = 0;
    esp_timer_handle_t miniapp_hold_timer_ = nullptr;
    gpio_num_t gpio_num_ = GPIO_NUM_NC;
    button_handle_t button_handle_ = nullptr;

    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
    std::unordered_map<uint8_t, std::function<void()>> on_multiple_click_;
    std::function<void(uint8_t)> on_multiple_click_at_least_;
    uint8_t min_multiple_click_count_ = 0;
};

#if CONFIG_SOC_ADC_SUPPORTED
class AdcButton : public Button {
public:
    AdcButton(const button_adc_config_t& adc_config);
};
#endif

class PowerSaveButton : public Button {
public:
    PowerSaveButton(gpio_num_t gpio_num) : Button(gpio_num, false, 0, 0, true) {
    }
};

#endif // BUTTON_H_
