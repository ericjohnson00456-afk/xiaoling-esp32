#include "watchdog.h"

#include <esp_log.h>

#define TAG "Watchdog"

Watchdog::Watchdog(uint32_t max_timeout_ms, const char* name)
: max_timeout_ms_(max_timeout_ms)
, name_(name) {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            Watchdog* wd = (Watchdog*)arg;
            wd->OnTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wd_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &timer_);
}

Watchdog::~Watchdog() {
    Stop();
    if (timer_ != NULL) {
        esp_timer_delete(timer_);
        timer_ = NULL;
    }
}

void Watchdog::Start() {
    if (timer_ == NULL) {
        return;
    }

    if (is_started_.exchange(true)) {
        return;
    }

    ESP_LOGI(TAG, "Started %s with max timeout %" PRIu32 " ms", name_, max_timeout_ms_);

    tte_ms_ = 0;
    last_feed_time_ms_ = esp_timer_get_time() / 1000;

    esp_timer_start_periodic(timer_, 1000 * 1000); // 1 second
}

void Watchdog::Stop() {
    esp_timer_stop(timer_);
    is_started_ = false;
    ESP_LOGI(TAG, "Stopped %s", name_);
}

void Watchdog::Feed(uint32_t duration_ms) {
    if (timer_ == NULL) {
        return;
    }

    if (!is_started_) {
        return;
    }

    if (first_feed_.exchange(false)) {
        tte_ms_ = 0;
    }

    Tick();
    tte_ms_ += duration_ms;

    // ESP_LOGI(TAG, "Fed %s with %" PRIu32 " ms, tte: %" PRIi32 " ms", name_, duration_ms, tte_ms_);
}

void Watchdog::Tick() {
    const int32_t now_ms = esp_timer_get_time() / 1000;
    const int32_t elapsed_ms = now_ms - last_feed_time_ms_;
    last_feed_time_ms_ = now_ms;
    tte_ms_ -= elapsed_ms;
}

void Watchdog::OnTimer() {
    Tick();
    if (tte_ms_ <= -(int32_t)max_timeout_ms_) {
        ESP_LOGW(TAG, "Stall timeout occurred on %s, tte: %" PRIi32 " ms", name_, tte_ms_);
        auto cb = on_timeout_;
        if (cb) {
            cb();
        }
        Stop();
    } else {
        // ESP_LOGI(TAG, "Checking %s tte: %" PRIi32 " ms", name_, tte_ms_);
    }
}
