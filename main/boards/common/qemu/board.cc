// Test-only board boundary. Production builds do not compile or link this file.
// Physical board constructors/drivers are not exercised by this fixture.
#include "board.h"
#include "codecs/dummy_audio_codec.h"
#include "event/app_event.h"

#include <esp_eth.h>
#include <esp_heap_caps.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_network.h>
#include <freertos/task.h>

#include <cstring>
#include <utility>

#if CONFIG_IDF_TARGET_ESP32P4
#include <esp_wifi_types.h>
#endif

namespace {
constexpr char kTag[] = "QemuBoard";
constexpr uint32_t kIdentity = XIAOLING_QEMU_ID;
constexpr uint8_t kSimulatedMac[] = {
    0x02, 0x71, static_cast<uint8_t>(kIdentity >> 24),
    static_cast<uint8_t>(kIdentity >> 16), static_cast<uint8_t>(kIdentity >> 8),
    static_cast<uint8_t>(kIdentity)};

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

class QemuBoard final : public Board {
public:
    QemuBoard() {
        ESP_LOGW(kTag, "PROFILE=%s TARGET=%s; TEST ONLY: virtual peripherals, real cloud",
                 BOARD_NAME, CONFIG_IDF_TARGET);
        ESP_ERROR_CHECK(esp_base_mac_addr_set(kSimulatedMac));
#if CONFIG_IDF_TARGET_ESP32C5
        ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(
            [](size_t bytes, uint32_t caps, const char* function) {
                if (caps & MALLOC_CAP_SPIRAM) return;  // Expected external-stack fallback in this model.
                ESP_LOGE(kTag, "QEMU_ALLOC_FAILED: %s bytes=%u caps=0x%lx free=%u largest=%u",
                         function, static_cast<unsigned>(bytes), static_cast<unsigned long>(caps),
                         static_cast<unsigned>(heap_caps_get_free_size(caps)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
            }));
#endif
#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_SPIRAM
        // Validate actual read/write backing, rather than accepting only the
        // size reported by the early simulation bridge.
        auto* probe = static_cast<uint32_t*>(heap_caps_malloc(65536, MALLOC_CAP_SPIRAM));
        ESP_ERROR_CHECK(probe ? ESP_OK : ESP_ERR_NO_MEM);
        for (size_t i = 0; i < 65536 / sizeof(uint32_t); ++i) probe[i] = i ^ 0xa55a3cc3;
        for (size_t i = 0; i < 65536 / sizeof(uint32_t); ++i) {
            ESP_ERROR_CHECK(probe[i] == (i ^ 0xa55a3cc3) ? ESP_OK : ESP_FAIL);
        }
        heap_caps_free(probe);
        ESP_LOGW(kTag, "QEMU_P4_PSRAM_WINDOW_OK: 32MB mapped RAM; chip init virtualized");
#endif
        const auto connected = AppEvent::Subscribe(AppEvent::Id::XiaolingConnected,
            [](const AppEvent::Event&) { ESP_LOGI(kTag, "QEMU_CLOUD_CONNECTED"); });
        const auto disconnected = AppEvent::Subscribe(AppEvent::Id::XiaolingDisconnected,
            [](const AppEvent::Event&) { ESP_LOGW(kTag, "QEMU_CLOUD_DISCONNECTED"); });
        ESP_ERROR_CHECK(connected != 0 && disconnected != 0 ? ESP_OK : ESP_ERR_NO_MEM);
    }
    std::string GetBoardType() override { return "wifi"; }
    AudioCodec* GetAudioCodec() override {
        static QemuAudioCodec codec;
        return &codec;
    }
    NetworkInterface* GetNetwork() override {
        static EspNetwork network;
        return &network;
    }
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        callback_ = std::move(callback);
    }
    const char* GetNetworkStateIcon() override { return ""; }
    void SetPowerSaveLevel(PowerSaveLevel) override {}
    std::string GetDeviceStatusJson() override { return "{}"; }
    std::string GetBoardJson() override {
        return std::string("{\"type\":\"wifi\",\"name\":\"") + BOARD_NAME +
               "\",\"qemu_fixture\":true}";
    }

    void StartNetwork() override {
        ESP_ERROR_CHECK(esp_netif_init());
        auto result = esp_event_loop_create_default();
        if (result != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(result);
        esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
        netif_ = esp_netif_new(&netif_config);
        ESP_ERROR_CHECK(netif_ != nullptr ? ESP_OK : ESP_ERR_NO_MEM);
        eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
        eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
        phy_config.phy_addr = 1;
        phy_config.reset_gpio_num = -1;
        phy_config.autonego_timeout_ms = 100;
        auto* mac = esp_eth_mac_new_openeth(&mac_config);
        auto* phy = esp_eth_phy_new_dp83848(&phy_config);
        ESP_ERROR_CHECK(mac && phy ? ESP_OK : ESP_ERR_NO_MEM);
        esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
        ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_));
        ESP_ERROR_CHECK(esp_eth_ioctl(eth_, ETH_CMD_S_MAC_ADDR,
                                     const_cast<uint8_t*>(kSimulatedMac)));
        auto glue = esp_eth_new_netif_glue(eth_);
        ESP_ERROR_CHECK(glue != nullptr ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(esp_netif_attach(netif_, glue));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, GotIp, this));
        ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED,
                                                   Disconnected, this));
        ESP_ERROR_CHECK(esp_eth_start(eth_));
    }
private:
    esp_netif_t* netif_ = nullptr;
    esp_eth_handle_t eth_ = nullptr;
    NetworkEventCallback callback_;
    static void GotIp(void* context, esp_event_base_t, int32_t, void* data) {
        auto* board = static_cast<QemuBoard*>(context);
        if (static_cast<ip_event_got_ip_t*>(data)->esp_netif != board->netif_) return;
        ESP_LOGI(kTag, "QEMU_NETWORK_READY");
        if (board->callback_) board->callback_(NetworkEvent::Connected, "qemu-openeth");
    }
    static void Disconnected(void* context, esp_event_base_t, int32_t, void*) {
        auto* board = static_cast<QemuBoard*>(context);
        if (board->callback_) board->callback_(NetworkEvent::Disconnected, "qemu-openeth");
    }
};
}  // namespace

extern "C" void* __wrap__Z12create_boardv() { return new QemuBoard(); }

#if CONFIG_IDF_TARGET_ESP32P4
extern "C" esp_err_t __wrap_esp_wifi_get_mac(wifi_interface_t, uint8_t* mac) {
    if (!mac) return ESP_ERR_INVALID_ARG;
    std::memcpy(mac, kSimulatedMac, sizeof(kSimulatedMac));
    return ESP_OK;
}
#endif
