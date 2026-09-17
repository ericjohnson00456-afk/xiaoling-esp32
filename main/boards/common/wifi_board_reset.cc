#include "wifi_board.h"

#include "board_events.h"

#include <ssid_manager.h>
#include <wifi_manager.h>

bool WifiBoard::ClearNetworkConfiguration() {
    WifiManager::GetInstance().StopStation();
    SsidManager::GetInstance().Clear();
    if (IsInWifiConfigMode()) {
        return true;
    }
    // The normal BLE entry reboots; provision in the current boot instead.
    BoardEvents::PostInput(BoardEvents::Input::WifiConfigNotification);
    StartWifiConfigMode();
    return true;
}
