#pragma once

#include "miniapp.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_MINIAPP
bool miniapp_store_supported(void);
bool miniapp_store_has_app(void);
/* Save only validated candidates. A failed write preserves the previous slot.
 * Boards without a source partition keep their existing RAM-only installation. */
const char *miniapp_store_save(const miniapp_package_t *package, const char *source);
/* Returns a PSRAM snapshot owned by the caller (free), including after exit/reboot. */
const char *miniapp_store_load(miniapp_package_t *package, char **source);
/* Blocks further writes until reboot, including in-flight installation attempts. */
bool miniapp_store_clear(void);
#else
static inline bool miniapp_store_supported(void) { return false; }
static inline bool miniapp_store_clear(void) { return true; }
#endif

#ifdef __cplusplus
}
#endif
