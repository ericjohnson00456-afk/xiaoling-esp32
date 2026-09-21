#ifndef ADC_BATTERY_MONITOR_H
#define ADC_BATTERY_MONITOR_H

#include <functional>
#include <driver/gpio.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>
#include <adc_battery_estimation.h>
#include <esp_timer.h>

class AdcBatteryMonitor {
public:
    AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel, float upper_resistor, float lower_resistor, gpio_num_t charging_pin = GPIO_NUM_NC);
    ~AdcBatteryMonitor();

    bool IsCharging();
    bool IsDischarging();
    uint8_t GetBatteryLevel();
    float GetBatteryVoltage();

    void OnChargingStatusChanged(std::function<void(bool)> callback);

private:
    gpio_num_t charging_pin_;
    adc_channel_t adc_channel_;
    float voltage_divider_ratio_;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    bool adc_cali_handle_created_ = false;
    adc_battery_estimation_handle_t adc_battery_estimation_handle_ = nullptr;
    esp_timer_handle_t timer_handle_ = nullptr;
    bool is_charging_ = false;
    std::function<void(bool)> on_charging_status_changed_;

    void CheckBatteryStatus();
};

#endif // ADC_BATTERY_MONITOR_H
