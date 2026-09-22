#pragma once

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Marks a socket write as latency-sensitive.  The project-side esp-ml307
// transport override reads this scope and applies a hard, per-call deadline
// without changing the timeout of receive operations on the same TLS socket.
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

// Kept as a C symbol so the transport component can consult the application
// deadline without depending on the main component's C++ headers.
extern "C" int xiaoling_transport_current_send_timeout_ms();
