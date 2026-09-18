#pragma once

#include "observable_model.h"

#include <string>

struct TimeModelState {
    std::string text;
    bool valid = false;
};

class TimeModel final : public ObservableModel<TimeModelState> {
public:
    static TimeModel& GetInstance();
    void Refresh();

private:
    TimeModel() = default;
};
