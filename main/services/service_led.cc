#include "service_led.h"

#include <cassert>

#include <esp_log.h>

#include "application.h"

namespace {
constexpr int kFastBlinkIntervalMs = 100;
constexpr int kBlinkIntervalMs = 500;
constexpr int kSlowBlinkIntervalMs = 1000;
constexpr int64_t kMicrosecondsPerMillisecond = 1000;
constexpr char kTag[] = "LedService";
} // namespace

LedService::LedService(gpio_num_t gpio, bool output_invert)
    : gpio_(gpio), output_invert_(output_invert) {
  assert(GPIO_IS_VALID_OUTPUT_GPIO(gpio_));

  const gpio_config_t config = {
      .pin_bit_mask = 1ULL << gpio_,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&config));
  SetLevel(false);

  const esp_timer_create_args_t timer_args = {
      .callback =
          [](void *arg) { static_cast<LedService *>(arg)->OnBlinkTimer(); },
      .arg = this,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "led_blink",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &blink_timer_));
}

LedService::~LedService() {
  if (blink_timer_ != nullptr) {
    esp_timer_stop(blink_timer_);
    esp_timer_delete(blink_timer_);
  }
  SetLevel(false);
}

void LedService::TurnOn() {
  std::lock_guard<std::mutex> lock(mutex_);
  StopBlink();
  SetLevel(true);
}

void LedService::TurnOff() {
  std::lock_guard<std::mutex> lock(mutex_);
  StopBlink();
  SetLevel(false);
}

void LedService::FastBlink() { StartBlink(kFastBlinkIntervalMs); }

void LedService::Blink() { StartBlink(kBlinkIntervalMs); }

void LedService::SlowBlink() { StartBlink(kSlowBlinkIntervalMs); }

void LedService::StartBlink(int interval_ms) {
  if (interval_ms <= 0) {
    ESP_LOGE(kTag, "Blink interval must be positive: %d", interval_ms);
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  StopBlink();
  SetLevel(true);
  ESP_ERROR_CHECK(esp_timer_start_periodic(blink_timer_,
                                           static_cast<uint64_t>(interval_ms) *
                                               kMicrosecondsPerMillisecond));
}

void LedService::StopBlink() {
  if (blink_timer_ != nullptr) {
    esp_timer_stop(blink_timer_);
  }
}

void LedService::SetLevel(bool on) {
  is_on_ = on;
  const int level = output_invert_ ? !on : on;
  ESP_ERROR_CHECK(gpio_set_level(gpio_, level));
}

void LedService::OnBlinkTimer() {
  std::lock_guard<std::mutex> lock(mutex_);
  SetLevel(!is_on_);
}

void LedService::OnStateChanged() {
  switch (Application::GetInstance().GetDeviceState()) {
  case kDeviceStateStarting:
  case kDeviceStateUpgrading:
    FastBlink();
    break;
  case kDeviceStateWifiConfiguring:
  case kDeviceStateActivating:
    Blink();
    break;
  case kDeviceStateIdle:
    TurnOff();
    break;
  case kDeviceStateConnecting:
  case kDeviceStateListening:
  case kDeviceStateAudioTesting:
  case kDeviceStateSpeaking:
    TurnOn();
    break;
  default:
    ESP_LOGW(kTag, "Unknown LED state");
    break;
  }
}
