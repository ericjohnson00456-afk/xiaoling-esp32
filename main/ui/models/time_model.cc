#include "time_model.h"

#include "time_sync.h"

#include <ctime>
#include <utility>

TimeModel& TimeModel::GetInstance() {
    static TimeModel instance;
    return instance;
}

void TimeModel::Refresh() {
    TimeModelState state;
    const std::time_t now = std::time(nullptr);
    std::tm local_time = {};
    if (TimeSync::IsSystemTimeValid() && localtime_r(&now, &local_time) != nullptr) {
        char buffer[16] = {};
        std::strftime(buffer, sizeof(buffer), "%H:%M", &local_time);
        state.text = buffer;
        state.valid = true;
    }

    const auto previous = Snapshot();
    if (state.valid != previous.valid || state.text != previous.text) {
        Publish(std::move(state));
    }
}
