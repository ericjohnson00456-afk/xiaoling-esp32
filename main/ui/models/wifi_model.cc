#include "wifi_model.h"

#include "board.h"

#include <utility>

WifiModel& WifiModel::GetInstance() {
    static WifiModel instance;
    return instance;
}

void WifiModel::Refresh() {
    const char* icon = Board::GetInstance().GetNetworkStateIcon();
    WifiModelState state;
    if (icon != nullptr) {
        state.icon = icon;
    }
    if (state.icon != Snapshot().icon) {
        Publish(std::move(state));
    }
}
