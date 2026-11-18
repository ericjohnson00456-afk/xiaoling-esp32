#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniapp_synth.h"

#ifdef __cplusplus
extern "C" {
#endif
bool miniapp_platform_busy(void);
bool miniapp_platform_alarm(void);
void miniapp_platform_mode_changed(bool active);
void miniapp_platform_interrupt(void);
bool miniapp_platform_tts_claim(uint32_t id);
void miniapp_platform_tts_cancel(uint32_t id);
void miniapp_platform_tts_release(uint32_t id);
void miniapp_platform_tts(const char *text, uint32_t id, const char **result, const char **error);
void miniapp_platform_sound(const miniapp_score_t *score, bool loop, uint16_t hz, uint16_t ms,
                            uint32_t generation, const volatile uint32_t *current,
                            const volatile bool *paused);
bool miniapp_time_valid(void);
int miniapp_get_time(struct timeval *tv);
int miniapp_localtime(struct tm *tm);
int miniapp_utc_offset(void);
void miniapp_led_on(void);
void miniapp_led_off(void);
void miniapp_led_blink(uint32_t on, uint32_t off);
#ifdef __cplusplus
}
#endif
static inline void *miniapp_alloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static inline void *miniapp_calloc(size_t n, size_t size) {
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static inline void *miniapp_realloc(void *ptr, size_t size) {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
