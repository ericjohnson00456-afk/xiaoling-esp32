#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <network_interface.h>
#include <driver/gpio.h>
#include <memory>

#include "services/service_led.h"
#include "backlight.h"
#include "camera.h"
#include "image_fetcher.h"
#include "resource/assets.h"
#include "display_hardware.h"

/**
 * Network events for unified callback
 */
enum class NetworkEvent {
    Scanning,              // Network is scanning (WiFi scanning, etc.)
    Connecting,            // Network is connecting (data: SSID/network name)
    Connected,             // Network connected successfully (data: SSID/network name)
    Disconnected,          // Network disconnected
    WifiConfigModeEnter,   // Entered WiFi configuration mode
    WifiConfigModeExit,    // Exited WiFi configuration mode
    // Cellular modem specific events
    ModemDetecting,        // Detecting modem (baud rate, module type)
    ModemErrorNoSim,       // No SIM card detected
    ModemErrorRegDenied,   // Network registration denied
    ModemErrorInitFailed,  // Modem initialization failed
    ModemErrorTimeout      // Operation timeout
};

// Power save level enumeration
enum class PowerSaveLevel {
    LOW_POWER,    // Maximum power saving (lowest power consumption)
    BALANCED,     // Medium power saving (balanced)
    PERFORMANCE,  // No power saving (maximum power consumption / full performance)
};

// Network event callback type (event, data)
// data contains additional info like SSID for Connecting/Connected events
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

void* create_board();
class AudioCodec;
class Display;
class McpServer;
class LvglImage;
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    virtual Display* GetDisplay();
    // Boards that only initialize a panel expose hardware handles here. UI
    // creates and owns the concrete Display implementation.
    virtual DisplayHardwareDescriptor GetDisplayHardware() const { return {}; }
    void SetDisplay(Display* display, int width = 0, int height = 0,
                    bool monochrome = false) {
        display_ = display;
        display_width_ = width;
        display_height_ = height;
        display_monochrome_ = monochrome;
    }
    virtual Camera* GetCamera();
#if CONFIG_LSPLATFORM
    virtual ImageFetcher* GetImageFetcher();
    virtual std::unique_ptr<LvglImage> GetProvisioningQRCode(std::string& message) {
        (void)message;
        return nullptr;
    }
#endif // CONFIG_LSPLATFORM
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetSystemInfoJson();
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;

    // Register MCP tools that are specific to this hardware variant. Common
    // tools are registered by Application before this hook is called.
    virtual void RegisterMcpTools(McpServer& server) { (void)server; }

    // Board input is published by the hardware implementation and interpreted
    // by Application.  These hooks keep the application independent of a
    // concrete WiFi/dual-network board when it needs to request a network
    // action in response to a button event.
    virtual bool EnterNetworkConfigMode() { return false; }
    virtual void SwitchNetworkType() {}

private:
    Display* display_ = nullptr;
    int display_width_ = 0;
    int display_height_ = 0;
    bool display_monochrome_ = false;
#if CONFIG_LSPLATFORM
    ImageFetcher* image_fetcher_ = nullptr;
#endif // CONFIG_LSPLATFORM
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
