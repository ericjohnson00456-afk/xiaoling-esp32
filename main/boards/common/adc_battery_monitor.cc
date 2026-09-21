#include "adc_battery_monitor.h"

#include <esp_log.h>

namespace {

constexpr const char* TAG = "AdcBatteryMonitor";

}  // namespace

AdcBatteryMonitor::AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel, float upper_resistor, float lower_resistor, gpio_num_t charging_pin)
    : charging_pin_(charging_pin),
      adc_channel_(adc_channel),
      voltage_divider_ratio_(lower_resistor / (upper_resistor + lower_resistor)) {
    
    // Initialize charging pin (only if it's not NC)
    if (charging_pin_ != GPIO_NUM_NC) {
        gpio_config_t gpio_cfg = {
            .pin_bit_mask = 1ULL << charging_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&gpio_cfg));
    }

    // Own the ADC handles in the application. The managed battery component
    // accepts external handles, which lets this class also read the voltage
    // without changing the managed component's public API.
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = adc_unit,
    };
    if (adc_oneshot_new_unit(&init_cfg, &adc_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ADC unit");
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(adc_handle_, adc_channel_, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel");
        adc_oneshot_del_unit(adc_handle_);
        adc_handle_ = nullptr;
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = adc_unit,
        .chan = adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle_) == ESP_OK) {
        adc_cali_handle_created_ = true;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = adc_unit,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle_) == ESP_OK) {
        adc_cali_handle_created_ = true;
    }
#endif

    if (!adc_cali_handle_created_) {
        ESP_LOGE(TAG, "Failed to create ADC calibration scheme");
        adc_oneshot_del_unit(adc_handle_);
        adc_handle_ = nullptr;
        return;
    }

    adc_battery_estimation_t adc_cfg = {
        .external = {
            .adc_handle = adc_handle_,
            .adc_cali_handle = adc_cali_handle_,
        },
        .adc_channel = adc_channel_,
        .upper_resistor = upper_resistor,
        .lower_resistor = lower_resistor
    };

    // 在ADC配置部分进行条件设置
    if (charging_pin_ != GPIO_NUM_NC) {
        adc_cfg.charging_detect_cb = [](void *user_data) -> bool {
            AdcBatteryMonitor *self = (AdcBatteryMonitor *)user_data;
            return gpio_get_level(self->charging_pin_) == 1;
        };
        adc_cfg.charging_detect_user_data = this;
    } else {
        // 不设置回调，让adc_battery_estimation库使用软件估算
        adc_cfg.charging_detect_cb = nullptr;
        adc_cfg.charging_detect_user_data = nullptr;
    }
    adc_battery_estimation_handle_ = adc_battery_estimation_create(&adc_cfg);
    if (adc_battery_estimation_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create battery estimator");
    }

    // Initialize timer
    esp_timer_create_args_t timer_cfg = {
        .callback = [](void *arg) {
            AdcBatteryMonitor *self = (AdcBatteryMonitor *)arg;
            self->CheckBatteryStatus();
        },
        .arg = this,
        .name = "adc_battery_monitor",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_cfg, &timer_handle_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));
}

AdcBatteryMonitor::~AdcBatteryMonitor() {
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
    }

    if (adc_battery_estimation_handle_) {
        ESP_ERROR_CHECK(adc_battery_estimation_destroy(adc_battery_estimation_handle_));
    }

    if (adc_cali_handle_created_) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(adc_cali_handle_));
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(adc_cali_handle_));
#endif
    }
    if (adc_handle_) {
        ESP_ERROR_CHECK(adc_oneshot_del_unit(adc_handle_));
    }
}

bool AdcBatteryMonitor::IsCharging() {
    // 优先使用adc_battery_estimation库的功能
    if (adc_battery_estimation_handle_ != nullptr) {
        bool is_charging = false;
        esp_err_t err = adc_battery_estimation_get_charging_state(adc_battery_estimation_handle_, &is_charging);
        if (err == ESP_OK) {
            return is_charging;
        }
    }
    
    // 回退到GPIO读取或返回默认值
    if (charging_pin_ != GPIO_NUM_NC) {
        return gpio_get_level(charging_pin_) == 1;
    }
    
    return false;
}

bool AdcBatteryMonitor::IsDischarging() {
    return !IsCharging();
}

uint8_t AdcBatteryMonitor::GetBatteryLevel() {
    // 如果句柄无效，返回默认值
    if (adc_battery_estimation_handle_ == nullptr) {
        return 100;
    }
    
    float capacity = 0;
    esp_err_t err = adc_battery_estimation_get_capacity(adc_battery_estimation_handle_, &capacity);
    if (err != ESP_OK) {
        return 100; // 出错时返回默认值
    }
    return (uint8_t)capacity;
}

float AdcBatteryMonitor::GetBatteryVoltage() {
    if (adc_handle_ == nullptr || adc_cali_handle_ == nullptr || voltage_divider_ratio_ <= 0.0f) {
        return 0.0f;
    }

    int raw = 0;
    int adc_voltage_mv = 0;
    if (adc_oneshot_read(adc_handle_, adc_channel_, &raw) != ESP_OK ||
        adc_cali_raw_to_voltage(adc_cali_handle_, raw, &adc_voltage_mv) != ESP_OK) {
        return 0.0f;
    }
    return (static_cast<float>(adc_voltage_mv) / 1000.0f) / voltage_divider_ratio_;
}

void AdcBatteryMonitor::OnChargingStatusChanged(std::function<void(bool)> callback) {
    on_charging_status_changed_ = callback;
}

void AdcBatteryMonitor::CheckBatteryStatus() {
    bool new_charging_status = IsCharging();
    if (new_charging_status != is_charging_) {
        is_charging_ = new_charging_status;
        if (on_charging_status_changed_) {
            on_charging_status_changed_(is_charging_);
        }
    }
}
