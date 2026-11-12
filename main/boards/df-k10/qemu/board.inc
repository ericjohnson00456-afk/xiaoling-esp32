// QEMU supplies OpenCores Ethernet, but no K10 I2C/SPI/I2S/camera/RMT devices.
// This fixture exercises the normal Application and XiaolingConn over real
// sockets. It does not validate the hardware implementation of Df_K10Board.
#include "wifi_board.h"
#include "codecs/dummy_audio_codec.h"
#include "event/app_event.h"

#include <esp_eth.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_network.h>
#include <freertos/task.h>

#if !CONFIG_ETH_USE_OPENETH || !CONFIG_IDF_TARGET_ESP32S3
#error "DF-K10 QEMU fixture requires ESP32-S3 and CONFIG_ETH_USE_OPENETH=y"
#endif

namespace {

class QemuAudioCodec final : public DummyAudioCodec {
public:
    QemuAudioCodec() : DummyAudioCodec(16000, 16000) {}

private:
    int Read(int16_t*, int) override {
        vTaskDelay(pdMS_TO_TICKS(20));
        return 0;
    }
    int Write(const int16_t*, int samples) override {
        vTaskDelay(pdMS_TO_TICKS(20));
        return samples;
    }
};

class DfK10QemuBoard final : public WifiBoard {
public:
    DfK10QemuBoard() {
        ESP_LOGW("DF-K10-QEMU", "TEST FIXTURE: hardware peripherals replaced; real cloud transport");
        // Synthetic identity for this fixture, independent of any physical K10.
        const uint8_t simulated_mac[6] = {0x02, 0x71, 0xdf, 0x10, 0x00, 0x01};
        ESP_ERROR_CHECK(esp_base_mac_addr_set(simulated_mac));
        const auto connected = AppEvent::Subscribe(AppEvent::Id::XiaolingConnected,
            [](const AppEvent::Event&) {
                ESP_LOGI("DF-K10-QEMU", "DF_K10_QEMU_CLOUD_CONNECTED");
            });
        const auto disconnected = AppEvent::Subscribe(AppEvent::Id::XiaolingDisconnected,
            [](const AppEvent::Event&) {
                ESP_LOGW("DF-K10-QEMU", "DF_K10_QEMU_CLOUD_DISCONNECTED");
            });
        ESP_ERROR_CHECK(connected != 0 && disconnected != 0 ? ESP_OK : ESP_ERR_NO_MEM);
    }

    AudioCodec* GetAudioCodec() override {
        static QemuAudioCodec codec;
        return &codec;
    }
    NetworkInterface* GetNetwork() override {
        static EspNetwork network;
        return &network;
    }
    const char* GetNetworkStateIcon() override { return ""; }
    void SetPowerSaveLevel(PowerSaveLevel) override {}
    std::string GetDeviceStatusJson() override { return "{}"; }
    bool EnterNetworkConfigMode() override { return false; }

    void StartNetwork() override {
        ESP_ERROR_CHECK(esp_netif_init());
        const auto result = esp_event_loop_create_default();
        if (result != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(result);
        esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
        netif_ = esp_netif_new(&netif_config);
        ESP_ERROR_CHECK(netif_ != nullptr ? ESP_OK : ESP_ERR_NO_MEM);

        eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
        eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
        phy_config.phy_addr = 1;  // OpenCores model's DEFAULT_PHY
        phy_config.reset_gpio_num = -1;
        phy_config.autonego_timeout_ms = 100;
        auto* mac = esp_eth_mac_new_openeth(&mac_config);
        auto* phy = esp_eth_phy_new_dp83848(&phy_config);
        ESP_ERROR_CHECK(mac != nullptr && phy != nullptr ? ESP_OK : ESP_ERR_NO_MEM);
        esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
        ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_));
        uint8_t address[6];
        ESP_ERROR_CHECK(esp_read_mac(address, ESP_MAC_ETH));
        ESP_ERROR_CHECK(esp_eth_ioctl(eth_, ETH_CMD_S_MAC_ADDR, address));
        auto glue = esp_eth_new_netif_glue(eth_);
        ESP_ERROR_CHECK(glue != nullptr ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(esp_netif_attach(netif_, glue));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                   GotIp, this));
        ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED,
                                                   Disconnected, this));
        ESP_ERROR_CHECK(esp_eth_start(eth_));
    }

protected:
    std::string GetBoardJson() override {
        return "{\"type\":\"wifi\",\"name\":\"df-k10\",\"qemu_fixture\":true}";
    }

private:
    esp_netif_t* netif_ = nullptr;
    esp_eth_handle_t eth_ = nullptr;

    static void GotIp(void* context, esp_event_base_t, int32_t, void* data) {
        auto* board = static_cast<DfK10QemuBoard*>(context);
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        if (event->esp_netif != board->netif_) return;
        ESP_LOGI("DF-K10-QEMU", "Virtual Ethernet acquired IPv4");
        if (board->network_event_callback_) {
            board->network_event_callback_(NetworkEvent::Connected, "qemu-openeth");
        }
    }
    static void Disconnected(void* context, esp_event_base_t, int32_t, void*) {
        auto* board = static_cast<DfK10QemuBoard*>(context);
        if (board->network_event_callback_) {
            board->network_event_callback_(NetworkEvent::Disconnected, "qemu-openeth");
        }
    }
};

}  // namespace

DECLARE_BOARD(DfK10QemuBoard);
