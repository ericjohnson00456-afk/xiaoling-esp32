#pragma once

#include "observable_model.h"

#include <string>

struct WifiModelState {
    std::string icon;
};

class WifiModel final : public ObservableModel<WifiModelState> {
public:
    static WifiModel& GetInstance();
    void Refresh();

private:
    WifiModel() = default;
};
