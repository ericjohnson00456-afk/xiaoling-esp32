#ifndef XIAOLING_MINIAPP_H
#define XIAOLING_MINIAPP_H

#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MINIAPP_API_VERSION 5
#define MINIAPP_SCREEN_WIDTH miniapp_screen_width()
#define MINIAPP_SCREEN_HEIGHT miniapp_screen_height()
#define MINIAPP_TEXT_FONT_PX 16
#define MINIAPP_SOURCE_MAX CONFIG_MINIAPP_SOURCE_MAX_BYTES
#define MINIAPP_MAX_SHAPES 256
#define MINIAPP_MAX_TEXTS 16
#define MINIAPP_TEXT_MAX 63
#define MINIAPP_SYNTH_QUEUE_DEPTH 4
#define MINIAPP_SYNTH_MAX_DURATION_MS 60000
#define MINIAPP_LUA_HEAP_LIMIT CONFIG_MINIAPP_HEAP_MAX_BYTES
#define MINIAPP_TICK_MS 20u
#define MINIAPP_CHUNK_INSTRUCTION_LIMIT 500000u
#define MINIAPP_CHUNK_DEADLINE_MS 150u
#define MINIAPP_CALLBACK_INSTRUCTION_LIMIT 250000u
/* Allow occasional response parsing to exceed the tick interval. Runtime
 * statistics discount task preemption; the instruction limit stays bounded. */
#define MINIAPP_CALLBACK_DEADLINE_MS 500u
#define MINIAPP_BUTTON_EXIT_HOLD_MS 3000u
#define MINIAPP_BUZZER_MIN_HZ 100u
#define MINIAPP_BUZZER_MAX_HZ 5000u
#define MINIAPP_BUZZER_MIN_MS 20u
#define MINIAPP_BUZZER_MAX_MS 3000u
#define MINIAPP_TTS_TEXT_MAX 512u
#define MINIAPP_TTS_MAX_PENDING 1u
#define MINIAPP_TTS_TIMEOUT_MS 120000u
#define MINIAPP_AUDIO_STATE_MAX 16u
#define MINIAPP_HTTP_MAX_URL_BYTES 511u
#define MINIAPP_HTTP_MAX_REQUESTS 4u
#define MINIAPP_HTTP_DEFAULT_TIMEOUT_MS 10000u
#define MINIAPP_HTTP_MAX_TIMEOUT_MS 60000u
#define MINIAPP_HTTP_DEFAULT_RESPONSE_BYTES 8192u
#define MINIAPP_HTTP_MAX_RESPONSE_BYTES 32768u
#define MINIAPP_HTTP_MAX_HEADERS 16u
#define MINIAPP_HTTP_MAX_HEADER_BYTES 1024u
#define MINIAPP_HTTP_MAX_BODY_BYTES 8192u
#define MINIAPP_HAS_SCREEN 1
#define MINIAPP_HAS_BUTTONS 1
#define MINIAPP_HAS_LEDS 1
#define MINIAPP_HAS_BUZZER 1

#ifdef __cplusplus
extern "C" {
#endif
int miniapp_screen_width(void);
int miniapp_screen_height(void);
bool miniapp_has_screen(void);
bool miniapp_has_speaker(void);
bool miniapp_has_led(void);
bool miniapp_button_known(const char *id);
struct cJSON *miniapp_capabilities(void);
bool miniapp_identity(char *id, size_t id_size, char *version, size_t version_size);
bool miniapp_ready(void);
typedef enum {
    MINIAPP_SHAPE_RECT,
    MINIAPP_SHAPE_CIRCLE,
    MINIAPP_SHAPE_LINE,
    MINIAPP_SHAPE_ARC,
} miniapp_shape_kind_t;

typedef struct {
    int16_t x;
    int16_t y;
    /* Rectangle size, line endpoint, or circle/arc radius in w. */
    int16_t w;
    int16_t h;
    uint32_t color;
    uint32_t border_color;
    uint16_t radius;
    uint16_t width;
    uint16_t start_angle;
    uint16_t end_angle;
    uint16_t order;
    uint8_t kind;
    uint8_t opacity;
    uint8_t fill;
    uint8_t rounded;
} miniapp_shape_t;

typedef struct {
    int16_t x;
    int16_t y;
    uint32_t color;
    uint16_t width;
    uint16_t order;
    uint8_t align;
    uint8_t opacity;
    char text[MINIAPP_TEXT_MAX + 1];
} miniapp_text_t;

typedef struct {
    uint32_t background;
    uint16_t shape_count;
    uint16_t text_count;
    bool ordered;
    miniapp_shape_t shapes[MINIAPP_MAX_SHAPES];
    miniapp_text_t texts[MINIAPP_MAX_TEXTS];
} miniapp_scene_t;

/* Copy only live commands. Expanded budgets must not turn an empty frame
 * into a full-budget memory transfer on every Lua tick. */
static inline void miniapp_scene_copy(miniapp_scene_t *dst, const miniapp_scene_t *src) {
    dst->background = src->background;
    dst->shape_count = src->shape_count;
    dst->text_count = src->text_count;
    dst->ordered = src->ordered;
    memcpy(dst->shapes, src->shapes, src->shape_count * sizeof(src->shapes[0]));
    memcpy(dst->texts, src->texts, src->text_count * sizeof(src->texts[0]));
}

typedef struct {
    bool installed;
    bool active;
    uint32_t source_size;
    uint32_t source_hash;
    uint32_t frames;
    uint32_t dropped_frames;
    uint32_t last_frame_ms;
    uint32_t max_frame_ms;
    char last_error[128];
} miniapp_status_t;

/* Strings are bounded UTF-8 bytes; identifiers are opaque, never paths. */
#define MINIAPP_ID_MAX 127
#define MINIAPP_VERSION_MAX 127
#define MINIAPP_NAME_MAX 127

typedef struct {
    char id[MINIAPP_ID_MAX + 1];
    char version[MINIAPP_VERSION_MAX + 1];
    char name[MINIAPP_NAME_MAX + 1];
    char hash[33];
    uint32_t size;
} miniapp_package_t;

#ifdef CONFIG_MINIAPP
int miniapp_init(void);
bool miniapp_is_active(void);
int miniapp_button_click(const char *button_id);
int miniapp_exit(void);
/* Queue a restore only from the desktop, without replacing a running app. */
int miniapp_reopen(void);
/* One install at a time, including its download. Never queue a second install. */
bool miniapp_install_begin(void);
void miniapp_install_end(void);
bool miniapp_matches(const miniapp_package_t *package);
int miniapp_install(const miniapp_package_t *package, const char *source, char *error,
                    size_t error_size);
/* Synchronous RPC to the Lua task. Caller owns returned JSON (cJSON_Delete).
 * Never call from the Lua/UI tasks. NULL is reserved for allocation failure. */
struct cJSON;
struct cJSON *miniapp_describe(char *error, size_t error_size);
struct cJSON *miniapp_action(const char *action, const struct cJSON *arguments, char *error,
                             size_t error_size);
#else
static inline bool miniapp_is_active(void) {
    return false;
}
#endif

int miniapp_ui_init(void);
int miniapp_ui_open(void);
int miniapp_ui_close(void);
int miniapp_ui_present(const miniapp_scene_t *scene);
int miniapp_ui_wait_frame(uint32_t timeout_ms);
void miniapp_runtime_ui_closed(void);

#ifdef __cplusplus
}
#endif
#endif
