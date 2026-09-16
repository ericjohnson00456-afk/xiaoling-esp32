#ifndef XIAOLING_SERVICES_LED_H_
#define XIAOLING_SERVICES_LED_H_

#include <mutex>

#include <driver/gpio.h>
#include <esp_timer.h>

class Led {
public:
  virtual ~Led() = default;

  virtual void OnStateChanged() = 0;
  virtual void TurnOn() {}
  virtual void TurnOff() {}
  virtual void FastBlink() {}
  virtual void Blink() {}
  virtual void SlowBlink() {}
};

class NoLed : public Led {
public:
  void OnStateChanged() override {}
};

class LedService : public Led {
public:
  explicit LedService(gpio_num_t gpio, bool output_invert = false);
  ~LedService() override;

  void OnStateChanged() override;
  void TurnOn() override;
  void TurnOff() override;
  void FastBlink() override;
  void Blink() override;
  void SlowBlink() override;

private:
  gpio_num_t gpio_;
  bool output_invert_;
  bool is_on_ = false;
  esp_timer_handle_t blink_timer_ = nullptr;
  std::mutex mutex_;

  void StartBlink(int interval_ms);
  void StopBlink();
  void SetLevel(bool on);
  void OnBlinkTimer();
};

#endif // XIAOLING_SERVICES_LED_H_
