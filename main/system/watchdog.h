#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <functional>
#include <atomic>

#include <stdint.h>
#include <esp_timer.h>

class Watchdog {
public:
    Watchdog(uint32_t max_timeout_ms, const char* name = "watchdog");
    ~Watchdog();

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    Watchdog(Watchdog&&) = delete;
    Watchdog& operator=(Watchdog&&) = delete;

    void Start();
    void Stop();
    bool IsRunning() const { return is_started_; }
    void Feed(uint32_t duration_ms);

    void OnTimeout(std::function<void()> callback) { on_timeout_ = callback; }

private:
    void Tick();
    void OnTimer();

    uint32_t max_timeout_ms_;
    const char* name_;

    esp_timer_handle_t timer_{nullptr};
    std::atomic<bool> is_started_{false};

    int32_t tte_ms_{0};
    int32_t last_feed_time_ms_{0};
    std::atomic<bool> first_feed_{true};

    std::function<void()> on_timeout_;
};

#endif // WATCHDOG_H
