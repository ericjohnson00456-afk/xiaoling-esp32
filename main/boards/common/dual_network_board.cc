#include "dual_network_board.h"
#include "board_events.h"
#include "settings.h"
#include <esp_log.h>

static const char *TAG = "DualNetworkBoard";

DualNetworkBoard::DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin, gpio_num_t ml307_dtr_pin, int32_t default_net_type)
    : Board(),
      ml307_tx_pin_(ml307_tx_pin),
      ml307_rx_pin_(ml307_rx_pin),
      ml307_dtr_pin_(ml307_dtr_pin) {

    // 从Settings加载网络类型
    network_type_ = LoadNetworkTypeFromSettings(default_net_type);

    // 只初始化当前网络类型对应的板卡
    InitializeCurrentBoard();
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    Settings settings("network", true);
    int network_type = settings.GetInt("type", default_net_type); // 默认使用ML307 (1)
    return network_type == 1 ? NetworkType::ML307 : NetworkType::WIFI;
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type = (type == NetworkType::ML307) ? 1 : 0;
    settings.SetInt("type", network_type);
}

void DualNetworkBoard::InitializeCurrentBoard() {
    if (network_type_ == NetworkType::ML307) {
        ESP_LOGI(TAG, "Initialize ML307 board");
        current_board_ = std::make_unique<Ml307Board>(ml307_tx_pin_, ml307_rx_pin_, ml307_dtr_pin_);
    } else {
        ESP_LOGI(TAG, "Initialize WiFi board");
        current_board_ = std::make_unique<WifiBoard>();
    }
}

void DualNetworkBoard::SwitchNetworkType() {
    if (network_type_ == NetworkType::WIFI) {
        SaveNetworkTypeToSettings(NetworkType::ML307);
        BoardEvents::PostInput(BoardEvents::Input::NetworkSwitchTo4G);
    } else {
        SaveNetworkTypeToSettings(NetworkType::WIFI);
        BoardEvents::PostInput(BoardEvents::Input::NetworkSwitchToWifi);
    }
    BoardEvents::PostInput(BoardEvents::Input::RebootRequested);
}

bool DualNetworkBoard::EnterNetworkConfigMode() {
    if (network_type_ != NetworkType::WIFI) {
        return false;
    }
    auto* wifi_board = dynamic_cast<WifiBoard*>(current_board_.get());
    if (wifi_board == nullptr) {
        return false;
    }
    return wifi_board->EnterNetworkConfigMode();
}

std::string DualNetworkBoard::GetBoardType() {
    return current_board_->GetBoardType();
}

void DualNetworkBoard::StartNetwork() {
    current_board_->StartNetwork();
}

void DualNetworkBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    // Forward the callback to the current board
    current_board_->SetNetworkEventCallback(std::move(callback));
}

NetworkInterface* DualNetworkBoard::GetNetwork() {
    return current_board_->GetNetwork();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return current_board_->GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    current_board_->SetPowerSaveLevel(level);
}

std::string DualNetworkBoard::GetBoardJson() {
    return current_board_->GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return current_board_->GetDeviceStatusJson();
}
