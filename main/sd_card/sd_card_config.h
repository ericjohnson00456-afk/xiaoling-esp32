#pragma once

#include <cstdint>

#include <driver/gpio.h>
#include <driver/spi_common.h>

// 版型只需要提供 SPI 接线和可选的电源控制方式。
// SD 卡挂载、扫描和热插拔轮询由 SdCardManager 统一负责。
struct SdSpiCardConfig {
    spi_host_device_t host = SPI2_HOST;
    gpio_num_t mosi = GPIO_NUM_NC;
    gpio_num_t miso = GPIO_NUM_NC;
    gpio_num_t sclk = GPIO_NUM_NC;
    gpio_num_t cs = GPIO_NUM_NC;
    gpio_num_t power = GPIO_NUM_NC;
    uint8_t power_on_level = 0;
};
