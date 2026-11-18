#include "led.h"
#include "sdkconfig.h"
#if CONFIG_MINIAPP
#include <esp_timer.h>
#include <mutex>

namespace {
std::mutex mutex;
esp_timer_handle_t timer = nullptr;
Led *target = nullptr;
bool illuminated = false;
unsigned on_ms = 0, off_ms = 0;
void Tick(void *) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!target)
        return;
    illuminated = !illuminated;
    target->SetMiniappOutput(illuminated);
    esp_timer_start_once(timer, static_cast<uint64_t>(illuminated ? on_ms : off_ms) * 1000);
}
} // namespace
#endif
void Led::SetMiniapp(bool enabled, unsigned on, unsigned off) {
#if CONFIG_MINIAPP
    if (!SupportsMiniapp())
        return;
    std::lock_guard<std::mutex> lock(mutex);
    if (timer)
        esp_timer_stop(timer);
    target = this;
    illuminated = enabled;
    on_ms = on;
    off_ms = off;
    SetMiniappOutput(illuminated);
    if (illuminated && on_ms && off_ms) {
        if (!timer) {
            const esp_timer_create_args_t args{.callback = Tick,
                                               .arg = nullptr,
                                               .dispatch_method = ESP_TIMER_TASK,
                                               .name = "miniapp_led"};
            if (esp_timer_create(&args, &timer) != ESP_OK)
                return;
        }
        esp_timer_start_once(timer, static_cast<uint64_t>(on_ms) * 1000);
    }
#else
    (void)enabled;
    (void)on;
    (void)off;
#endif
}
