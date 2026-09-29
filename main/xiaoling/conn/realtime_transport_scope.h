#pragma once

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Marks a socket write as latency-sensitive. The application-side lwIP send
// wrapper reads this scope and applies a per-call deadline without changing
// receive timeouts or managed component sources.
class RealtimeTransportScope final {
public:
    explicit RealtimeTransportScope(uint32_t timeout_ms);
    ~RealtimeTransportScope();

    RealtimeTransportScope(const RealtimeTransportScope&) = delete;
    RealtimeTransportScope& operator=(const RealtimeTransportScope&) = delete;

private:
    TaskHandle_t task_ = nullptr;
    TaskHandle_t previous_task_ = nullptr;
    uint32_t previous_timeout_ms_ = 0;
};

// Kept as a C symbol so the lwIP send wrapper can consult the application
// deadline without exposing the C++ scope type.
extern "C" int xiaoling_transport_current_send_timeout_ms();
