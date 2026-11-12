// Test-only bridge to Espressif QEMU's existing 0x48000000 RAM window.
// The current machine backs that window with RAM but does not implement the
// HEX-PSRAM mode-register identification protocol. This does NOT test the real
// PSRAM chip/controller initialization and must never enter a hardware build.
#include <esp_attr.h>
#include <esp_err.h>
#include <stdint.h>
#include <sdkconfig.h>

#if !CONFIG_IDF_TARGET_ESP32P4 || !CONFIG_SPIRAM
#error "This simulation bridge is only for P4 with external RAM enabled"
#endif

esp_err_t IRAM_ATTR __wrap_esp_psram_impl_enable(void) {
    return ESP_OK;
}

esp_err_t IRAM_ATTR __wrap_esp_psram_impl_get_physical_size(uint32_t* size) {
    if (!size) return ESP_ERR_INVALID_ARG;
    *size = 32U * 1024U * 1024U;
    return ESP_OK;
}

esp_err_t IRAM_ATTR __wrap_esp_psram_impl_get_available_size(uint32_t* size) {
    if (!size) return ESP_ERR_INVALID_ARG;
    *size = 32U * 1024U * 1024U;
#if CONFIG_SPIRAM_ECC_ENABLE
    *size = *size * 7 / 8;
#endif
    return ESP_OK;
}
