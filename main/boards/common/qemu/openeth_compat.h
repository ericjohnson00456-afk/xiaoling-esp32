#pragma once
#include "sdkconfig.h"
#include "soc/soc.h"
#include "soc/interrupts.h"

// Espressif QEMU esp32c5_reg.h / esp32c6_reg.h, release 20261109.
#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6
#define DR_REG_EMAC_BASE 0x600CD000
#define ETS_ETH_MAC_INTR_SOURCE ETS_WIFI_MAC_INTR_SOURCE
#endif
