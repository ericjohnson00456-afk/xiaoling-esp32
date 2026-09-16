#ifndef XIAOLING_SERVICES_WS2812_H_
#define XIAOLING_SERVICES_WS2812_H_

#include "service_led.h"
#include <driver/gpio.h>
#include <led_strip.h>
#include <esp_timer.h>
#include <atomic>
#include <mutex>

class Ws2812Service : public Led {
public:
    explicit Ws2812Service(gpio_num_t gpio);
    ~Ws2812Service() override;

    void OnStateChanged() override;
    void TurnOn() override;
    void TurnOff() override;
    void FastBlink() override;
    void Blink() override;
    void SlowBlink() override;

private:
    std::mutex mutex_;
    TaskHandle_t blink_task_ = nullptr;
    led_strip_handle_t led_strip_ = nullptr;
    uint8_t r_ = 0, g_ = 0, b_ = 0;
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    esp_timer_handle_t blink_timer_ = nullptr;

    void StartBlinkTask(int times, int interval_ms);
    void OnBlinkTimer();

    void BlinkOnce();
    void Blink(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
    void SetColor(uint8_t r, uint8_t g, uint8_t b);
};

#endif  // XIAOLING_SERVICES_WS2812_H_
