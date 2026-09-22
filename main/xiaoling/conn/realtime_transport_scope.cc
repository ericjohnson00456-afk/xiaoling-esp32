#include "realtime_transport_scope.h"

#include <atomic>

namespace {

std::atomic<TaskHandle_t> s_realtime_writer{nullptr};
std::atomic<uint32_t> s_realtime_timeout_ms{0};

}  // namespace

RealtimeTransportScope::RealtimeTransportScope(uint32_t timeout_ms)
    : task_(xTaskGetCurrentTaskHandle()),
      previous_task_(s_realtime_writer.exchange(task_)),
      previous_timeout_ms_(s_realtime_timeout_ms.exchange(timeout_ms)) {
}

RealtimeTransportScope::~RealtimeTransportScope() {
    s_realtime_timeout_ms.store(previous_timeout_ms_);
    s_realtime_writer.store(previous_task_);
}

extern "C" int xiaoling_transport_current_send_timeout_ms() {
    if (s_realtime_writer.load() != xTaskGetCurrentTaskHandle()) {
        return 0;
    }
    return static_cast<int>(s_realtime_timeout_ms.load());
}
