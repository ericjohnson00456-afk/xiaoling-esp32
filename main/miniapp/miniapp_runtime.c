#include "miniapp.h"
#include "miniapp_synth.h"
#include "miniapp_audio.h"
#include "miniapp_lua_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "cJSON.h"
#include "miniapp_storage.h"
#include "miniapp_store.h"
#include "miniapp_actions.h"
#include "miniapp_platform.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#define TAG "miniapp"
#define MINIAPP_HOOK_GRANULARITY 1000u

typedef enum {
    MINIAPP_EVENT_INSTALL,
    MINIAPP_EVENT_REOPEN,
    MINIAPP_EVENT_CLICK,
    MINIAPP_EVENT_EXIT,
    MINIAPP_EVENT_DESCRIBE,
    MINIAPP_EVENT_ACTION,
} miniapp_event_type_t;

typedef struct {
    miniapp_event_type_t type;
    char button_id[16];
    void *request;
    uint32_t generation;
} miniapp_event_t;

typedef struct {
    uint32_t speech_id;
    uint32_t generation;
    char text[MINIAPP_TTS_TEXT_MAX + 1];
    char url[512];
    bool url_ready, synth_failed, cancelled, complete, started;
    const char *result;
    const char *error;
} miniapp_tts_job_t;

/* One audible request, plus bounded asynchronous busy/error results. */
#define MINIAPP_TTS_RESULT_SLOTS 4u

typedef struct {
    miniapp_score_t *score; /* Owned by pending VM, queue, then audio worker. */
    bool loop;
    uint16_t frequency_hz;
    uint16_t duration_ms;
    uint32_t generation;
} miniapp_buzzer_command_t;

typedef struct miniapp_http_request miniapp_http_request_t;

typedef struct {
    miniapp_http_request_t *request;
    int error;
    unsigned status;
    char content_type[128];
    char error_code[24];
    char error_message[64];
    char *body;
    size_t body_len;
} miniapp_http_response_t;

struct miniapp_http_request {
    uint32_t id;
    uint32_t generation;
    bool cancelled;
    bool queued;
    char url[MINIAPP_HTTP_MAX_URL_BYTES + 1];
    char headers[MINIAPP_HTTP_MAX_HEADER_BYTES + 1u];
    char *body;
    size_t body_len;
    esp_http_client_method_t method;
    uint32_t timeout_ms;
    size_t max_response_bytes;
    bool complete;
    miniapp_http_response_t response;
};

typedef struct {
    size_t used;
    size_t limit;
    uint32_t instruction_left;
    TickType_t deadline;
#if configGENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    configRUN_TIME_COUNTER_TYPE cpu_started;
    configRUN_TIME_COUNTER_TYPE cpu_limit;
#endif
    bool validating;
    uint32_t http_generation;
    bool synth_paused;
    bool faulted, retiring, storage_read;
    bool exiting;
    cJSON *actions;
    int action_ref;
    char id[MINIAPP_ID_MAX + 1];
    cJSON *storage_json;
    char storage_data[MINIAPP_STORAGE_MAX_BYTES + 1];
    size_t storage_size;
    uint32_t storage_ttl;
    unsigned storage_pending; /* 0: none, 1: save, 2: clear; startup transaction */
    uint32_t present_count;
    miniapp_scene_t scene;
    miniapp_scene_t presented_scene;
    int pending_led;
    uint32_t led_on_ms, led_off_ms;
    unsigned buzz_count;
    miniapp_buzzer_command_t buzz[MINIAPP_SYNTH_QUEUE_DEPTH];
    char audio_url[MINIAPP_AUDIO_MAX_URL_BYTES + 1];
    bool audio_paused;
    char audio_state[MINIAPP_AUDIO_STATE_MAX];
} miniapp_vm_t;

typedef struct {
    const miniapp_package_t *package;
    const char *source;
    SemaphoreHandle_t done;
    int result;
    bool reopening;
    char error[128];
} miniapp_request_t;

typedef struct {
    const char *action;
    const cJSON *arguments;
    cJSON *result;
    SemaphoreHandle_t done;
    char error[128];
} miniapp_control_request_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_event_queue;
static TaskHandle_t s_task;
static bool s_initializing, s_initialized;
static QueueHandle_t s_buzzer_queue;
static TaskHandle_t s_buzzer_task;
static QueueHandle_t s_http_queue;
static TaskHandle_t s_http_task;
static SemaphoreHandle_t s_http_lock;
static miniapp_http_request_t *s_http_requests[MINIAPP_HTTP_MAX_REQUESTS];
static uint32_t s_http_next_id = 1;
static uint32_t s_http_generation;
static miniapp_status_t s_status;
static miniapp_package_t s_current;
static volatile bool s_active;
static volatile bool s_install_busy;
static volatile uint32_t s_buzzer_generation;
static volatile bool s_synth_paused;
static uint32_t s_install_generation;
static uint32_t s_tts_next_id = 1;
static uint32_t s_tts_worker_id;
static miniapp_tts_job_t *s_tts_requests[MINIAPP_TTS_RESULT_SLOTS];

static void miniapp_tts_worker(void *argument) {
    miniapp_tts_job_t *job = argument;
    job->result = "failed";
    job->error = "unavailable";
    miniapp_platform_tts(job->text, job->speech_id, &job->result, &job->error);
    taskENTER_CRITICAL(&s_lock);
    if (s_tts_worker_id == job->speech_id)
        s_tts_worker_id = 0;
    job->complete = true;
    taskEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static miniapp_tts_job_t *miniapp_tts_take_result(miniapp_vm_t *vm, bool frozen) {
    miniapp_tts_job_t *result = NULL;
    taskENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < MINIAPP_TTS_RESULT_SLOTS; ++i) {
        miniapp_tts_job_t *job = s_tts_requests[i];
        if (job && job->complete &&
            (!frozen || job->cancelled || !vm || job->generation != vm->http_generation)) {
            result = job;
            s_tts_requests[i] = NULL;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return result;
}

static void miniapp_set_active(bool active) {
    s_active = active;
    s_status.active = active;
    miniapp_platform_mode_changed(active);
}

static void miniapp_set_error(const char *message) {
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s",
             message ? message : "unknown error");
    ESP_LOGE(TAG, "%s", s_status.last_error);
}

static void *miniapp_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    miniapp_vm_t *vm = ud;
    if (nsize == 0) {
        if (ptr) {
            free(ptr);
            vm->used = osize <= vm->used ? vm->used - osize : 0;
        }
        return NULL;
    }

    size_t old_size = ptr ? osize : 0;
    if (nsize > UINT32_MAX || vm->used > vm->limit ||
        (nsize > old_size && nsize - old_size > vm->limit - vm->used)) {
        return NULL;
    }

    void *next = ptr ? miniapp_realloc(ptr, (uint32_t)nsize) : miniapp_alloc((uint32_t)nsize);
    if (next) {
        vm->used = vm->used - old_size + nsize;
    }
    return next;
}

static miniapp_vm_t *miniapp_vm_get(lua_State *L) {
    return *(miniapp_vm_t **)lua_getextraspace(L);
}

static lua_Integer miniapp_integer(lua_State *L, int index, lua_Integer min, lua_Integer max) {
    lua_Integer value = luaL_checkinteger(L, index);
    if (value < min || value > max) {
        luaL_error(L, "argument %d is out of range", index);
    }
    return value;
}

static int miniapp_lua_screen_present(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    vm->present_count++;
    miniapp_scene_copy(&vm->presented_scene, &vm->scene);
    if (!vm->validating && miniapp_ui_present(&vm->scene) != 0) {
        s_status.dropped_frames++;
    }
    return 0;
}

static int miniapp_lua_led_on(lua_State *L) {
    if (strcmp(luaL_checkstring(L, 1), "status") != 0) {
        return luaL_error(L, "unknown LED id");
    }
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (vm->validating)
        vm->pending_led = 1;
    else
        miniapp_led_on();
    return 0;
}

static int miniapp_lua_led_off(lua_State *L) {
    if (strcmp(luaL_checkstring(L, 1), "status") != 0) {
        return luaL_error(L, "unknown LED id");
    }
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (vm->validating)
        vm->pending_led = 0;
    else
        miniapp_led_off();
    return 0;
}

static int miniapp_lua_led_blink(lua_State *L) {
    if (strcmp(luaL_checkstring(L, 1), "status") != 0) {
        return luaL_error(L, "unknown LED id");
    }
    uint32_t on_ms = (uint32_t)miniapp_integer(L, 2, 10, 5000);
    uint32_t off_ms = (uint32_t)miniapp_integer(L, 3, 10, 5000);
    if (on_ms < 10 || on_ms > 5000 || off_ms < 10 || off_ms > 5000) {
        return luaL_error(L, "blink interval must be 10..5000 ms");
    }
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (vm->validating) {
        vm->pending_led = 2;
        vm->led_on_ms = on_ms;
        vm->led_off_ms = off_ms;
    } else
        miniapp_led_blink(on_ms, off_ms);
    return 0;
}

static bool miniapp_buzzer_enqueue(const miniapp_buzzer_command_t *command) {
    /* Sound effects are best effort. Audio contention must not fault the Lua
     * application or replay stale effects after a spoken reply or alarm. */
    if (!s_buzzer_queue || !miniapp_has_speaker() ||
        (!command->score && (miniapp_platform_busy() || miniapp_platform_alarm())) ||
        xQueueSend(s_buzzer_queue, command, 0) != pdTRUE) {
        free(command->score);
        return false;
    }
    return true;
}

static void miniapp_audio_clear_queue(void) {
    miniapp_buzzer_command_t command;
    while (s_buzzer_queue && xQueueReceive(s_buzzer_queue, &command, 0) == pdTRUE) {
        free(command.score);
    }
}

static void miniapp_audio_clear_pending(miniapp_vm_t *vm) {
    for (unsigned i = 0; i < vm->buzz_count; ++i) {
        free(vm->buzz[i].score);
    }
    vm->buzz_count = 0;
}

/* Scratch is Lua-owned so any validation error releases it automatically. */
static lua_Integer miniapp_note_integer(lua_State *L, int index, const char *key, lua_Integer min,
                                        lua_Integer max) {
    lua_getfield(L, index, key);
    int exact = 0;
    lua_Integer value = lua_tointegerx(L, -1, &exact);
    if (!exact || value < min || value > max) {
        luaL_error(L, "note.%s must be an integer in %d..%d", key, (int)min, (int)max);
    }
    lua_pop(L, 1);
    return value;
}

static int miniapp_lua_synth_play(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    bool loop = false;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "loop");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TBOOLEAN);
        }
        loop = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    size_t count = lua_rawlen(L, 1);
    if (!count || count > MINIAPP_SYNTH_NOTES) {
        return luaL_error(L, "score requires 1..256 notes");
    }
    miniapp_score_t *scratch = lua_newuserdata(L, sizeof(*scratch));
    memset(scratch, 0, sizeof(*scratch));
    scratch->count = count;
    static const char *const waves[] = {"square", "triangle", "noise", NULL};
    for (size_t i = 0; i < count; ++i) {
        lua_rawgeti(L, 1, i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        int index = lua_gettop(L);
        miniapp_note_t *note = &scratch->notes[i];
        note->at = miniapp_note_integer(L, index, "at", 0, MINIAPP_SYNTH_MAX_DURATION_MS) * 16u;
        note->duration = miniapp_note_integer(L, index, "ms", 20, 3000) * 16u;
        note->hz = miniapp_note_integer(L, index, "hz", 50, 5000);
        note->voice = miniapp_note_integer(L, index, "voice", 1, MINIAPP_SYNTH_VOICES) - 1;
        note->volume = miniapp_note_integer(L, index, "volume", 0, 100);
        lua_getfield(L, index, "wave");
        note->wave = luaL_checkoption(L, -1, "square", waves);
        lua_pop(L, 2);
        uint32_t end = note->at + note->duration;
        if (end > MINIAPP_SYNTH_MAX_DURATION_MS * 16u) {
            return luaL_error(L, "score exceeds 60 seconds");
        }
        if (end > scratch->samples) {
            scratch->samples = end;
        }
    }
    /* Small bounded insertion sort, independent of Lua's event order. */
    for (size_t i = 1; i < count; ++i) {
        miniapp_note_t note = scratch->notes[i];
        size_t j = i;
        while (j && scratch->notes[j - 1].at > note.at) {
            scratch->notes[j] = scratch->notes[j - 1];
            --j;
        }
        scratch->notes[j] = note;
    }
    uint32_t ends[MINIAPP_SYNTH_VOICES] = {0};
    for (size_t i = 0; i < count; ++i) {
        miniapp_note_t *note = &scratch->notes[i];
        if (note->at < ends[note->voice]) {
            return luaL_error(L, "notes overlap on the same voice");
        }
        ends[note->voice] = note->at + note->duration;
    }
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (vm->retiring) {
        lua_pushboolean(L, false);
        return 1;
    }
    if (vm->validating && vm->buzz_count == MINIAPP_SYNTH_QUEUE_DEPTH) {
        lua_pushboolean(L, false);
        return 1;
    }
    miniapp_score_t *score = miniapp_alloc(sizeof(*score));
    if (!score) {
        return luaL_error(L, "not enough memory for synth score");
    }
    memcpy(score, scratch, sizeof(*score));
    miniapp_buzzer_command_t command = {
        .score = score, .loop = loop, .generation = s_buzzer_generation};
    if (vm->validating) {
        vm->buzz[vm->buzz_count++] = command;
    } else if (!miniapp_buzzer_enqueue(&command)) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int miniapp_lua_synth_pause(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (vm->retiring) {
        return 0;
    }
    vm->synth_paused = true;
    if (!vm->validating) {
        s_synth_paused = true;
    }
    return 0;
}

static int miniapp_lua_synth_resume(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (vm->retiring) {
        return 0;
    }
    vm->synth_paused = false;
    if (!vm->validating) {
        s_synth_paused = false;
    }
    return 0;
}

static int miniapp_lua_buzzer_play(lua_State *L) {
    uint32_t frequency_hz =
        (uint32_t)miniapp_integer(L, 1, MINIAPP_BUZZER_MIN_HZ, MINIAPP_BUZZER_MAX_HZ);
    uint32_t duration_ms =
        (uint32_t)miniapp_integer(L, 2, MINIAPP_BUZZER_MIN_MS, MINIAPP_BUZZER_MAX_MS);
    if (frequency_hz < MINIAPP_BUZZER_MIN_HZ || frequency_hz > MINIAPP_BUZZER_MAX_HZ) {
        return luaL_error(L, "buzzer frequency must be %u..%u Hz", MINIAPP_BUZZER_MIN_HZ,
                          MINIAPP_BUZZER_MAX_HZ);
    }
    if (duration_ms < MINIAPP_BUZZER_MIN_MS || duration_ms > MINIAPP_BUZZER_MAX_MS) {
        return luaL_error(L, "buzzer duration must be %u..%u ms", MINIAPP_BUZZER_MIN_MS,
                          MINIAPP_BUZZER_MAX_MS);
    }
    miniapp_vm_t *vm = miniapp_vm_get(L);
    miniapp_buzzer_command_t command = {
        .frequency_hz = (uint16_t)frequency_hz,
        .duration_ms = (uint16_t)duration_ms,
        .generation = s_buzzer_generation,
    };
    if (vm->validating) {
        if (vm->buzz_count == MINIAPP_SYNTH_QUEUE_DEPTH)
            return 0;
        vm->buzz[vm->buzz_count++] = command;
    } else {
        (void)miniapp_buzzer_enqueue(&command);
    }
    return 0;
}

/* Validation only reserves requests; playback starts after the VM commits. */
static void miniapp_tts_start(miniapp_tts_job_t *job) {
    if (job->complete || job->cancelled || job->started)
        return;
    job->started = true;
    taskENTER_CRITICAL(&s_lock);
    bool worker_busy = s_tts_worker_id != 0;
    taskEXIT_CRITICAL(&s_lock);
    if (worker_busy || miniapp_platform_busy() || miniapp_platform_alarm() ||
        !miniapp_platform_tts_claim(job->speech_id)) {
        job->result = "failed";
        job->error = "busy";
        job->complete = true;
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    s_tts_worker_id = job->speech_id;
    taskEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(miniapp_tts_worker, "miniapp.tts", 4096, job, 4, NULL) != pdPASS) {
        miniapp_platform_tts_cancel(job->speech_id);
        miniapp_platform_tts_release(job->speech_id);
        taskENTER_CRITICAL(&s_lock);
        s_tts_worker_id = 0;
        taskEXIT_CRITICAL(&s_lock);
        job->result = "failed";
        job->error = "unavailable";
        job->complete = true;
    }
}

static void miniapp_tts_activate(miniapp_vm_t *vm) {
    for (unsigned i = 0; i < MINIAPP_TTS_RESULT_SLOTS; ++i) {
        miniapp_tts_job_t *job = s_tts_requests[i];
        if (job && job->generation == vm->http_generation)
            miniapp_tts_start(job);
    }
}

static int miniapp_lua_tts_speak(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    size_t len = 0;
    const char *text = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &len) : NULL;
    const char *error = NULL;
    if (!text || !len || memchr(text, '\0', len))
        error = "invalid_argument";
    else if (len > MINIAPP_TTS_TEXT_MAX)
        error = "too_large";
    else if (vm->faulted || vm->retiring || vm->exiting)
        error = "unavailable";

    miniapp_tts_job_t *job = miniapp_calloc(1, sizeof(*job));
    if (!job)
        return luaL_error(L, "not enough memory for TTS result");
    if (!error)
        memcpy(job->text, text, len);
    job->generation = vm->http_generation;
    if (error) {
        job->result = "failed";
        job->error = error;
        job->complete = true;
    }
    taskENTER_CRITICAL(&s_lock);
    unsigned slot = MINIAPP_TTS_RESULT_SLOTS;
    for (unsigned i = 0; i < MINIAPP_TTS_RESULT_SLOTS; ++i)
        if (!s_tts_requests[i]) {
            slot = i;
            break;
        }
    if (slot == MINIAPP_TTS_RESULT_SLOTS) {
        taskEXIT_CRITICAL(&s_lock);
        free(job);
        return luaL_error(L, "too many pending TTS results");
    }
    uint32_t speech_id = s_tts_next_id++;
    if (!speech_id)
        speech_id = s_tts_next_id++;
    job->speech_id = speech_id;
    s_tts_requests[slot] = job;
    taskEXIT_CRITICAL(&s_lock);
    if (!vm->validating)
        miniapp_tts_start(job);
    lua_pushinteger(L, speech_id);
    return 1;
}

static int miniapp_lua_tts_cancel(lua_State *L) {
    lua_Integer requested = luaL_checkinteger(L, 1);
    if (requested < 1 || requested > UINT32_MAX)
        return luaL_error(L, "invalid speech id");
    bool accepted = false;
    taskENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < MINIAPP_TTS_RESULT_SLOTS; ++i) {
        miniapp_tts_job_t *job = s_tts_requests[i];
        if (job && job->speech_id == requested && job->generation == miniapp_vm_get(L)->http_generation &&
            !job->cancelled && !job->complete) {
            job->cancelled = true;
            if (!job->started)
                job->complete = true;
            accepted = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (accepted)
        miniapp_platform_tts_cancel((uint32_t)requested);
    lua_pushboolean(L, accepted);
    return 1;
}

static void miniapp_buzzer_task(void *argument) {
    (void)argument;
    miniapp_buzzer_command_t command;
    for (;;) {
        if (xQueueReceive(s_buzzer_queue, &command, portMAX_DELAY) != pdTRUE)
            continue;
        if (s_active && command.generation == s_buzzer_generation) {
            miniapp_platform_sound(command.score, command.loop, command.frequency_hz,
                                   command.duration_ms, command.generation, &s_buzzer_generation,
                                   &s_synth_paused);
        }
        free(command.score);
    }
}

static void miniapp_storage_fault(miniapp_vm_t *vm) {
    vm->faulted = true;
    vm->storage_pending = 0;
    cJSON_Delete(vm->storage_json);
    vm->storage_json = NULL;
    // A failed candidate must not erase the running application's save.
    if (vm->storage_read && !vm->validating) {
        const char *error = miniapp_storage_clear(vm->id);
        if (error)
            ESP_LOGW(TAG, "failed to clear app data: %s", error);
    }
}

static int miniapp_lua_clock_now(lua_State *L) {
    struct timeval tv;
    if (!miniapp_time_valid() || miniapp_get_time(&tv) != 0)
        lua_pushnil(L);
    else
        lua_pushinteger(L, tv.tv_sec);
    return 1;
}

static int miniapp_lua_clock_localtime(lua_State *L) {
    struct tm calendar;
    if (miniapp_localtime(&calendar) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 8);
#define CLOCK_FIELD(name, value)                                                                   \
    do {                                                                                           \
        lua_pushinteger(L, value);                                                                 \
        lua_setfield(L, -2, name);                                                                 \
    } while (0)
    CLOCK_FIELD("year", calendar.tm_year + 1900);
    CLOCK_FIELD("month", calendar.tm_mon + 1);
    CLOCK_FIELD("day", calendar.tm_mday);
    CLOCK_FIELD("hour", calendar.tm_hour);
    CLOCK_FIELD("min", calendar.tm_min);
    CLOCK_FIELD("sec", calendar.tm_sec);
    CLOCK_FIELD("wday", calendar.tm_wday);
    CLOCK_FIELD("utc_offset", miniapp_utc_offset());
#undef CLOCK_FIELD
    return 1;
}

static int miniapp_storage_result(lua_State *L, const char *error) {
    lua_pushboolean(L, error == NULL);
    if (error) {
        lua_pushstring(L, error);
        return 2;
    }
    return 1;
}

static bool miniapp_storage_object_valid(cJSON *object) {
    if (!cJSON_IsObject(object))
        return false;
    unsigned count = 0;
    for (cJSON *item = object->child; item; item = item->next) {
        if (++count > 32 || !item->string || !*item->string || strlen(item->string) > 32)
            return false;
        if (cJSON_IsNumber(item)) {
            if (!isfinite(item->valuedouble))
                return false;
        } else if (cJSON_IsString(item)) {
            if (strlen(item->valuestring) > 256)
                return false;
        } else if (!cJSON_IsBool(item))
            return false;
    }
    return true;
}

static int miniapp_lua_storage_load(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (vm->faulted || vm->storage_pending == 2 || !miniapp_time_valid()) {
        lua_pushnil(L);
        return 1;
    }
    size_t size = vm->storage_size;
    if (vm->storage_pending != 1 && !miniapp_storage_load(vm->id, vm->storage_data, &size)) {
        lua_pushnil(L);
        return 1;
    }
    vm->storage_json = cJSON_ParseWithLength(vm->storage_data, size + 1);
    if (!miniapp_storage_object_valid(vm->storage_json)) {
        cJSON_Delete(vm->storage_json);
        vm->storage_json = NULL;
        (void)miniapp_storage_clear(vm->id);
        lua_pushnil(L);
        return 1;
    }
    vm->storage_read = true;
    lua_newtable(L);
    for (cJSON *item = vm->storage_json->child; item; item = item->next) {
        if (cJSON_IsNumber(item))
            lua_pushnumber(L, item->valuedouble);
        else if (cJSON_IsBool(item))
            lua_pushboolean(L, cJSON_IsTrue(item));
        else
            lua_pushstring(L, item->valuestring);
        lua_setfield(L, -2, item->string);
    }
    cJSON_Delete(vm->storage_json);
    vm->storage_json = NULL;
    return 1;
}

static int miniapp_lua_storage_save(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (vm->faulted || vm->retiring)
        return miniapp_storage_result(L, "unavailable");
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer ttl = luaL_optinteger(L, 2, MINIAPP_STORAGE_DEFAULT_TTL);
    if (ttl < 1 || ttl > MINIAPP_STORAGE_MAX_TTL)
        return miniapp_storage_result(L, "invalid_ttl");
    if (!miniapp_time_valid())
        return miniapp_storage_result(L, "clock_unavailable");
    vm->storage_json = cJSON_CreateObject();
    if (!vm->storage_json)
        return miniapp_storage_result(L, "no_memory");
    const char *error = NULL;
    unsigned count = 0;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        size_t key_len = 0, value_len = 0;
        const char *key = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &key_len) : NULL;
        cJSON *item = NULL;
        if (++count > 32 || !key || !key_len || key_len > 32 || memchr(key, 0, key_len)) {
            error = "invalid_data";
        } else if (lua_type(L, -1) == LUA_TNUMBER && isfinite(lua_tonumber(L, -1))) {
            item = cJSON_CreateNumber(lua_tonumber(L, -1));
        } else if (lua_type(L, -1) == LUA_TBOOLEAN) {
            item = cJSON_CreateBool(lua_toboolean(L, -1));
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            const char *value = lua_tolstring(L, -1, &value_len);
            if (value_len > 256 || memchr(value, 0, value_len))
                error = "invalid_data";
            else
                item = cJSON_CreateString(value);
        } else
            error = "invalid_data";
        if (!error && (!item || !cJSON_AddItemToObject(vm->storage_json, key, item))) {
            cJSON_Delete(item);
            error = "no_memory";
        }
        lua_pop(L, 1);
        if (error) {
            lua_pop(L, 1);
            break;
        }
    }
    /* Never replace pending data on serialization failure. */
    char *encoded = !error ? cJSON_PrintUnformatted(vm->storage_json) : NULL;
    if (!error && !encoded)
        error = "no_memory";
    if (!error && strlen(encoded) > MINIAPP_STORAGE_MAX_BYTES)
        error = "too_large";
    if (!error) {
        if (vm->validating) {
            vm->storage_size = strlen(encoded);
            memcpy(vm->storage_data, encoded, vm->storage_size + 1);
            vm->storage_ttl = ttl;
            vm->storage_pending = 1;
        } else
            error = miniapp_storage_save(vm->id, encoded, strlen(encoded), ttl);
    }
    cJSON_free(encoded);
    cJSON_Delete(vm->storage_json);
    vm->storage_json = NULL;
    return miniapp_storage_result(L, error);
}

static int miniapp_lua_storage_clear(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (vm->faulted || vm->retiring)
        return miniapp_storage_result(L, "unavailable");
    if (vm->validating) {
        vm->storage_pending = 2;
        return miniapp_storage_result(L, NULL);
    }
    return miniapp_storage_result(L, miniapp_storage_clear(vm->id));
}

static bool miniapp_http_is_cancelled(miniapp_http_request_t *request) {
    bool cancelled;
    xSemaphoreTake(s_http_lock, portMAX_DELAY);
    cancelled = request->cancelled;
    xSemaphoreGive(s_http_lock);
    return cancelled;
}

static void miniapp_http_remove(miniapp_http_request_t *request) {
    xSemaphoreTake(s_http_lock, portMAX_DELAY);
    for (size_t i = 0; i < MINIAPP_HTTP_MAX_REQUESTS; ++i) {
        if (s_http_requests[i] == request)
            s_http_requests[i] = NULL;
    }
    xSemaphoreGive(s_http_lock);
}

static void miniapp_http_free(miniapp_http_request_t *request, miniapp_http_response_t *response) {
    if (response) {
        free(response->body);
    }
    if (request) {
        free(request->body);
        free(request);
    }
}

static void miniapp_http_error(miniapp_http_response_t *response, const char *code) {
    response->error = -1;
    snprintf(response->error_code, sizeof(response->error_code), "%s", code);
    snprintf(response->error_message, sizeof(response->error_message), "%s", code);
}

static esp_err_t miniapp_http_header_event(esp_http_client_event_t *event) {
    miniapp_http_response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        !strcasecmp(event->header_key, "Content-Type")) {
        snprintf(response->content_type, sizeof(response->content_type), "%s", event->header_value);
    }
    return ESP_OK;
}

static void miniapp_http_execute(miniapp_http_response_t *response) {
    miniapp_http_request_t *request = response->request;
    esp_http_client_config_t config = {
        .url = request->url,
        .method = request->method,
        .event_handler = miniapp_http_header_event,
        .user_data = response,
        .timeout_ms = request->timeout_ms,
        .disable_auto_redirect = true,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        miniapp_http_error(response, "unavailable");
        return;
    }
    for (char *line = request->headers; *line;) {
        char *colon = strchr(line, ':');
        char *end = strstr(line, "\r\n");
        if (!colon || !end || colon > end)
            break;
        *colon = 0;
        *end = 0;
        esp_http_client_set_header(client, line, colon + 2);
        *colon = ':';
        *end = '\r';
        line = end + 2;
    }
    TickType_t start = xTaskGetTickCount();
    esp_err_t rc = esp_http_client_open(client, request->body_len);
    if (rc != ESP_OK) {
        miniapp_http_error(response, "network_error");
        goto done;
    }
    size_t sent = 0;
    while (sent < request->body_len && !miniapp_http_is_cancelled(request)) {
        int n = esp_http_client_write(client, request->body + sent, request->body_len - sent);
        if (n <= 0) {
            miniapp_http_error(response, "network_error");
            goto done;
        }
        sent += n;
    }
    int64_t length = esp_http_client_fetch_headers(client);
    if (length < 0) {
        miniapp_http_error(response, "network_error");
        goto done;
    }
    response->status = esp_http_client_get_status_code(client);

    if ((uint64_t)length > request->max_response_bytes) {
        miniapp_http_error(response, "response_too_large");
        goto done;
    }
    for (;;) {
        if (miniapp_http_is_cancelled(request))
            break;
        uint32_t elapsed = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
        if (elapsed >= request->timeout_ms) {
            miniapp_http_error(response, "timeout");
            break;
        }
        esp_http_client_set_timeout_ms(client, request->timeout_ms - elapsed);
        char buffer[512];
        int n = esp_http_client_read(client, buffer, sizeof(buffer));
        if (n < 0) {
            miniapp_http_error(response, "network_error");
            break;
        }
        if (!n) {
            if (!esp_http_client_is_complete_data_received(client))
                miniapp_http_error(response, "network_error");
            break;
        }
        if ((size_t)n > request->max_response_bytes - response->body_len) {
            miniapp_http_error(response, "response_too_large");
            break;
        }
        memcpy(response->body + response->body_len, buffer, n);
        response->body_len += n;
        response->body[response->body_len] = 0;
    }
done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void miniapp_http_task(void *argument) {
    (void)argument;
    for (;;) {
        miniapp_http_request_t *request = NULL;
        if (xQueueReceive(s_http_queue, &request, portMAX_DELAY) != pdTRUE || !request)
            continue;
        if (miniapp_http_is_cancelled(request)) {
            miniapp_http_remove(request);
            miniapp_http_free(request, NULL);
            continue;
        }
        /* The result record was allocated with the accepted request. Even
         * response-body allocation failure has a durable error result. */
        miniapp_http_response_t *response = &request->response;
        response->request = request;
        response->body = miniapp_calloc(1, request->max_response_bytes + 1u);
        if (!response->body)
            miniapp_http_error(response, "unavailable");
        else
            miniapp_http_execute(response);
        ESP_LOGI(TAG, "HTTP result id=%u status=%u bytes=%u error=%s", (unsigned)request->id,
                 response->status, (unsigned)response->body_len, response->error_code);
        xSemaphoreTake(s_http_lock, portMAX_DELAY);
        request->complete = true;
        xSemaphoreGive(s_http_lock);
    }
}

/* Completed results remain owned by their request while the app is frozen. */
static miniapp_http_response_t *miniapp_http_take_result(miniapp_vm_t *vm, bool frozen) {
    miniapp_http_response_t *result = NULL;
    xSemaphoreTake(s_http_lock, portMAX_DELAY);
    for (unsigned i = 0; i < MINIAPP_HTTP_MAX_REQUESTS; ++i) {
        miniapp_http_request_t *r = s_http_requests[i];
        if (r && r->complete &&
            (!frozen || r->cancelled || !vm || r->generation != vm->http_generation)) {
            s_http_requests[i] = NULL;
            result = &r->response;
            break;
        }
    }
    xSemaphoreGive(s_http_lock);
    return result;
}

/* Candidate startup may create requests but cannot send them before commit.
 * Retiring one VM only cancels requests owned by that instance. */
static void miniapp_http_discard(miniapp_vm_t *vm) {
    xSemaphoreTake(s_http_lock, portMAX_DELAY);
    for (size_t i = 0; i < MINIAPP_HTTP_MAX_REQUESTS; ++i) {
        miniapp_http_request_t *r = s_http_requests[i];
        if (!r || r->generation != vm->http_generation)
            continue;
        r->cancelled = true;
        if (!r->queued || r->complete) {
            s_http_requests[i] = NULL;
            miniapp_http_free(r, &r->response);
        }
    }
    xSemaphoreGive(s_http_lock);
}

static void miniapp_http_activate(miniapp_vm_t *vm) {
    xSemaphoreTake(s_http_lock, portMAX_DELAY);
    for (size_t i = 0; i < MINIAPP_HTTP_MAX_REQUESTS; ++i) {
        miniapp_http_request_t *r = s_http_requests[i];
        if (!r || r->generation != vm->http_generation || r->queued)
            continue;
        /* Registry and worker queue have the same capacity. */
        r->queued = xQueueSend(s_http_queue, &r, 0) == pdTRUE;
    }
    xSemaphoreGive(s_http_lock);
}

static int miniapp_http_options(lua_State *L, int index, const char *url,
                                miniapp_http_request_t *request) {
    size_t url_len = strlen(url);
    if (!url_len || url_len > MINIAPP_HTTP_MAX_URL_BYTES ||
        (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) {
        return luaL_error(L, "invalid http URL");
    }
    snprintf(request->url, sizeof(request->url), "%s", url);
    request->method = HTTP_METHOD_GET;
    request->timeout_ms = MINIAPP_HTTP_DEFAULT_TIMEOUT_MS;
    request->max_response_bytes = MINIAPP_HTTP_DEFAULT_RESPONSE_BYTES;
    /* Omitted headers use the SDK defaults without a provider callback. */
    request->headers[0] = 0;
    if (index != 0 && !lua_isnoneornil(L, index)) {
        luaL_checktype(L, index, LUA_TTABLE);
        lua_getfield(L, index, "method");
        if (!lua_isnil(L, -1)) {
            const char *method = luaL_checkstring(L, -1);
            if (!strcmp(method, "POST"))
                request->method = HTTP_METHOD_POST;
            else if (strcmp(method, "GET")) {
                lua_pop(L, 1);
                return luaL_error(L, "method must be GET or POST");
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, index, "timeout_ms");
        if (!lua_isnil(L, -1)) {
            lua_Integer value = luaL_checkinteger(L, -1);
            if (value < 1 || value > MINIAPP_HTTP_MAX_TIMEOUT_MS) {
                lua_pop(L, 1);
                return luaL_error(L, "invalid timeout_ms");
            }
            request->timeout_ms = (uint32_t)value;
        }
        lua_pop(L, 1);
        lua_getfield(L, index, "max_response_bytes");
        if (!lua_isnil(L, -1)) {
            lua_Integer value = luaL_checkinteger(L, -1);
            if (value < 1 || value > MINIAPP_HTTP_MAX_RESPONSE_BYTES) {
                lua_pop(L, 1);
                return luaL_error(L, "invalid max_response_bytes");
            }
            request->max_response_bytes = (size_t)value;
        }
        lua_pop(L, 1);
        lua_getfield(L, index, "body");
        if (!lua_isnil(L, -1)) {
            size_t len = 0;
            const char *body = luaL_checklstring(L, -1, &len);
            if (request->method != HTTP_METHOD_POST || len > MINIAPP_HTTP_MAX_BODY_BYTES) {
                lua_pop(L, 1);
                return luaL_error(L, "body is only valid for POST and must be <= 8192 bytes");
            }
            request->body = miniapp_alloc(len + 1u);
            if (!request->body) {
                lua_pop(L, 1);
                return luaL_error(L, "no memory");
            }
            memcpy(request->body, body, len);
            request->body[len] = '\0';
            request->body_len = len;
        }
        lua_pop(L, 1);
        lua_getfield(L, index, "headers");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            size_t used = 0, count = 0;
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                size_t key_len = 0, value_len = 0;
                if (lua_type(L, -2) != LUA_TSTRING)
                    return luaL_error(L, "header names must be strings");
                const char *key = luaL_checklstring(L, -2, &key_len);
                const char *value = luaL_checklstring(L, -1, &value_len);
                if (memchr(key, 0, key_len) || strpbrk(key, "\r\n:") ||
                    memchr(value, 0, value_len) || strpbrk(value, "\r\n"))
                    return luaL_error(L, "invalid header characters");
                size_t add = key_len + value_len + 4u;
                if (++count > MINIAPP_HTTP_MAX_HEADERS || !key_len ||
                    used + add > MINIAPP_HTTP_MAX_HEADER_BYTES) {
                    lua_pop(L, 2);
                    return luaL_error(L, "invalid headers");
                }
                used +=
                    (size_t)snprintf(request->headers + used, sizeof(request->headers) - used,
                                     "%.*s: %.*s\r\n", (int)key_len, key, (int)value_len, value);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    if (request->method == HTTP_METHOD_POST && !request->body) {
        request->body = miniapp_calloc(1, 1);
    }
    return 0;
}

static int miniapp_http_options_protected(lua_State *L) {
    miniapp_http_request_t *request = lua_touserdata(L, 1);
    return miniapp_http_options(L, lua_isnil(L, 2) ? 0 : 2, lua_tostring(L, 3), request);
}

static int miniapp_lua_http_request_common(lua_State *L, int url_index, int options_index) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    url_index = lua_absindex(L, url_index);
    size_t url_len;
    const char *url = luaL_checklstring(L, url_index, &url_len);
    if (memchr(url, 0, url_len) || strpbrk(url, "\r\n"))
        return luaL_error(L, "invalid http URL");
    if (!lua_checkstack(L, 4))
        return luaL_error(L, "not enough Lua stack");
    if (vm->faulted || vm->retiring || vm->exiting || !s_http_queue)
        return luaL_error(L, "http unavailable");
    miniapp_http_request_t *request = miniapp_calloc(1, sizeof(*request));
    if (!request)
        return luaL_error(L, "no memory");
    lua_pushcfunction(L, miniapp_http_options_protected);
    lua_pushlightuserdata(L, request);
    if (options_index)
        lua_pushvalue(L, options_index);
    else
        lua_pushnil(L);
    lua_pushvalue(L, url_index);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        miniapp_http_free(request, NULL);
        return lua_error(L);
    }
    xSemaphoreTake(s_http_lock, portMAX_DELAY);
    size_t slot = MINIAPP_HTTP_MAX_REQUESTS;
    for (size_t i = 0; i < MINIAPP_HTTP_MAX_REQUESTS; ++i)
        if (!s_http_requests[i]) {
            slot = i;
            break;
        }
    if (slot == MINIAPP_HTTP_MAX_REQUESTS) {
        xSemaphoreGive(s_http_lock);
        miniapp_http_free(request, NULL);
        return luaL_error(L, "too many http requests");
    }
    request->id = s_http_next_id++;
    if (!request->id)
        request->id = s_http_next_id++;
    request->generation = vm->http_generation;
    s_http_requests[slot] = request;
    xSemaphoreGive(s_http_lock);
    uint32_t id = request->id;
    if (!vm->validating) {
        request->queued = true;
        if (xQueueSend(s_http_queue, &request, 0) != pdTRUE) {
            miniapp_http_remove(request);
            miniapp_http_free(request, NULL);
            return luaL_error(L, "http queue is busy");
        }
    }
    lua_pushinteger(L, id);
    return 1;
}

static int miniapp_lua_http_request(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "url");
    int result = miniapp_lua_http_request_common(L, -1, 1);
    lua_remove(L, -2); /* remove URL, preserve the returned request ID */
    return result;
}

static int miniapp_lua_http_get(lua_State *L) {
    return miniapp_lua_http_request_common(L, 1, lua_isnoneornil(L, 2) ? 0 : 2);
}

static int miniapp_lua_http_cancel(lua_State *L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    bool cancelled = false;
    xSemaphoreTake(s_http_lock, portMAX_DELAY);
    for (size_t i = 0; i < MINIAPP_HTTP_MAX_REQUESTS; ++i) {
        miniapp_http_request_t *r = s_http_requests[i];
        if (r && r->id == id && r->generation == miniapp_vm_get(L)->http_generation &&
            !r->cancelled) {
            r->cancelled = true;
            cancelled = true;
            if (!r->queued) {
                s_http_requests[i] = NULL;
                miniapp_http_free(r, NULL);
            }
            break;
        }
    }
    xSemaphoreGive(s_http_lock);
    lua_pushboolean(L, cancelled);
    return 1;
}

#define MINIAPP_JSON_MAX_DEPTH 32u
#define MINIAPP_JSON_MAX_BYTES MINIAPP_LUA_HEAP_LIMIT

static char null_key, array_key;
typedef struct {
    cJSON *root;
    char *printed;
    const char *error;
    const void *ancestors[MINIAPP_JSON_MAX_DEPTH + 1];
    size_t bytes;
    size_t limit; /* zero uses the general JSON API limit */
} json_call_t;

static void fill_json(lua_State *L, int index, cJSON *out, json_call_t *c, unsigned depth) {
    index = lua_absindex(L, index);
    size_t limit = c->limit ? c->limit : MINIAPP_JSON_MAX_BYTES;
    if (!lua_checkstack(L, 6)) {
        luaL_error(L, "not enough Lua stack");
        return;
    }
    if (c->bytes > limit - 8) {
        c->error = "too_large";
        return;
    }
    c->bytes += 8;
    if (depth > MINIAPP_JSON_MAX_DEPTH) {
        c->error = "too_deep";
        return;
    }
    if (lua_type(L, index) == LUA_TLIGHTUSERDATA && lua_touserdata(L, index) == &null_key) {
        out->type = cJSON_NULL;
    } else
        switch (lua_type(L, index)) {
        case LUA_TBOOLEAN:
            out->type = lua_toboolean(L, index) ? cJSON_True : cJSON_False;
            break;
        case LUA_TNUMBER:
            if (!isfinite(lua_tonumber(L, index))) {
                c->error = "invalid_value";
                break;
            }
            out->type = cJSON_Number;
            cJSON_SetNumberValue(out, lua_tonumber(L, index));
            break;
        case LUA_TSTRING: {
            size_t len;
            const char *s = lua_tolstring(L, index, &len);
            if (memchr(s, 0, len)) {
                c->error = "invalid_value";
                break;
            }
            if (len > limit - c->bytes) {
                c->error = "too_large";
                break;
            }
            c->bytes += len;
            out->valuestring = cJSON_malloc(len + 1);
            if (!out->valuestring) {
                c->error = "unavailable";
                break;
            }
            memcpy(out->valuestring, s, len + 1);
            out->type = cJSON_String;
            break;
        }
        case LUA_TTABLE: {
            const void *identity = lua_topointer(L, index);
            for (unsigned i = 0; i < depth; ++i)
                if (c->ancestors[i] == identity) {
                    c->error = "invalid_value";
                    return;
                }
            c->ancestors[depth] = identity;
            size_t count = 0, largest = 0;
            bool strings = false, numbers = false, array = false;
            if (lua_getmetatable(L, index)) {
                lua_rawgetp(L, LUA_REGISTRYINDEX, &array_key);
                array = lua_rawequal(L, -1, -2);
                lua_pop(L, 2);
            }
            lua_pushnil(L);
            while (lua_next(L, index)) {
                ++count;
                if (lua_type(L, -2) == LUA_TSTRING)
                    strings = true;
                else if (lua_isinteger(L, -2) && lua_tointeger(L, -2) > 0 &&
                         lua_tointeger(L, -2) <= MINIAPP_JSON_MAX_BYTES) {
                    numbers = true;
                    size_t n = (size_t)lua_tointeger(L, -2);
                    if (n > largest)
                        largest = n;
                } else
                    c->error = "invalid_value";
                lua_pop(L, 1);
            }
            if ((strings && numbers) || (numbers && largest != count))
                c->error = "invalid_value";
            if (c->error)
                break;
            array = numbers || (!count && array);
            out->type = array ? cJSON_Array : cJSON_Object;
            if (array) {
                for (size_t i = 1; i <= count && !c->error; ++i) {
                    cJSON *item = cJSON_CreateNull();
                    if (!item || !cJSON_AddItemToArray(out, item)) {
                        cJSON_Delete(item);
                        c->error = "unavailable";
                        break;
                    }
                    lua_rawgeti(L, index, i);
                    fill_json(L, -1, item, c, depth + 1);
                    lua_pop(L, 1);
                }
            } else {
                lua_pushnil(L);
                while (!c->error && lua_next(L, index)) {
                    size_t n;
                    const char *key = lua_tolstring(L, -2, &n);
                    if (memchr(key, 0, n)) {
                        c->error = "invalid_value";
                        break;
                    }
                    if (n > limit - c->bytes) {
                        c->error = "too_large";
                        break;
                    }
                    c->bytes += n;
                    cJSON *item = cJSON_CreateNull();
                    if (!item || !cJSON_AddItemToObject(out, key, item)) {
                        cJSON_Delete(item);
                        c->error = "unavailable";
                        break;
                    }
                    fill_json(L, -1, item, c, depth + 1);
                    lua_pop(L, 1);
                }
            }
            break;
        }
        default:
            c->error = "invalid_value";
        }
}

static void push_json(lua_State *L, const cJSON *item, unsigned depth) {
    if (!lua_checkstack(L, 6)) {
        luaL_error(L, "not enough Lua stack");
        return;
    }
    if (cJSON_IsNull(item))
        lua_pushlightuserdata(L, &null_key);
    else if (cJSON_IsBool(item))
        lua_pushboolean(L, cJSON_IsTrue(item));
    else if (cJSON_IsNumber(item))
        lua_pushnumber(L, item->valuedouble);
    else if (cJSON_IsString(item))
        lua_pushstring(L, item->valuestring);
    else {
        lua_newtable(L);
        bool array = cJSON_IsArray(item);
        if (array) {
            lua_rawgetp(L, LUA_REGISTRYINDEX, &array_key);
            lua_setmetatable(L, -2);
        }
        int n = 1;
        for (const cJSON *child = item->child; child; child = child->next) {
            push_json(L, child, depth + 1);
            if (array)
                lua_rawseti(L, -2, n++);
            else
                lua_setfield(L, -2, child->string);
        }
    }
}

static bool valid_tree(const cJSON *item, unsigned depth, json_call_t *c) {
    if (depth > MINIAPP_JSON_MAX_DEPTH) {
        c->error = "too_deep";
        return false;
    }
    if (cJSON_IsNumber(item) && !isfinite(item->valuedouble)) {
        c->error = "invalid_json";
        return false;
    }
    for (const cJSON *child = item->child; child; child = child->next)
        if (!valid_tree(child, depth + 1, c))
            return false;
    return true;
}

static int json_protected(lua_State *L) {
    json_call_t *c = lua_touserdata(L, 1);
    if (lua_toboolean(L, 3)) {
        size_t length;
        if (lua_type(L, 2) != LUA_TSTRING) {
            c->error = "invalid_json";
            return 0;
        }
        const char *text = lua_tolstring(L, 2, &length);
        if (memchr(text, 0, length)) {
            c->error = "invalid_json";
            return 0;
        }
        if (length > MINIAPP_JSON_MAX_BYTES || length > MINIAPP_LUA_HEAP_LIMIT) {
            c->error = "too_large";
            return 0;
        }
        /* Bound parser recursion before allocating cJSON nodes. Reject escaped
         * NUL explicitly: cJSON strings cannot represent embedded NUL bytes. */
        unsigned depth = 0;
        bool quoted = false;
        for (size_t i = 0; i < length; ++i) {
            if (quoted && text[i] == '\\') {
                if (length - i >= 6 && !memcmp(text + i, "\\u0000", 6)) {
                    c->error = "invalid_json";
                    return 0;
                }
                ++i;
            } else if (text[i] == '"')
                quoted = !quoted;
            else if (!quoted && (text[i] == '[' || text[i] == '{')) {
                if (++depth > MINIAPP_JSON_MAX_DEPTH + 1) {
                    c->error = "too_deep";
                    return 0;
                }
            } else if (!quoted && (text[i] == ']' || text[i] == '}') && depth)
                --depth;
        }
        const char *end = NULL;
        c->root = cJSON_ParseWithLengthOpts(text, length + 1, &end, true);
        if (!c->root) {
            c->error = "invalid_json";
            return 0;
        }
        if (valid_tree(c->root, 0, c)) {
            push_json(L, c->root, 0);
            return 1;
        }
    } else {
        c->root = cJSON_CreateNull();
        if (!c->root) {
            c->error = "unavailable";
            return 0;
        }
        fill_json(L, 2, c->root, c, 0);
        if (c->error)
            return 0;
        /* cJSON documents five bytes of extra space for preallocated output. */
        size_t capacity = c->bytes * 6 + 6;
        if (capacity > MINIAPP_JSON_MAX_BYTES + 6)
            capacity = MINIAPP_JSON_MAX_BYTES + 6;
        c->printed = miniapp_alloc(capacity);
        if (!c->printed) {
            c->error = "unavailable";
            return 0;
        }
        if (!cJSON_PrintPreallocated(c->root, c->printed, capacity, false) ||
            strlen(c->printed) > MINIAPP_JSON_MAX_BYTES ||
            strlen(c->printed) > MINIAPP_LUA_HEAP_LIMIT) {
            c->error = "too_large";
            return 0;
        }
        lua_pushstring(L, c->printed);
        return 1;
    }
    return 0;
}

static int json_call(lua_State *L, bool decode) {
    json_call_t c = {0};
    lua_pushcfunction(L, json_protected);
    lua_pushlightuserdata(L, &c);
    lua_pushvalue(L, 1);
    lua_pushboolean(L, decode);
    int result = lua_pcall(L, 3, 1, 0);
    cJSON_Delete(c.root);
    free(c.printed);
    if (result != LUA_OK)
        return lua_error(L);
    if (c.error) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, c.error);
        return 2;
    }
    return 1;
}
static int json_encode(lua_State *L) {
    return json_call(L, false);
}
static int json_decode(lua_State *L) {
    return json_call(L, true);
}

static void miniapp_json_open(lua_State *L) {
    lua_newtable(L);
    lua_pushliteral(L, "JSON array");
    lua_setfield(L, -2, "__metatable");
    lua_rawsetp(L, LUA_REGISTRYINDEX, &array_key);
    lua_newtable(L);
    lua_pushlightuserdata(L, &null_key);
    lua_setfield(L, -2, "null");
    lua_pushcfunction(L, json_encode);
    lua_setfield(L, -2, "encode");
    lua_pushcfunction(L, json_decode);
    lua_setfield(L, -2, "decode");
    lua_setglobal(L, "json");
}

static void miniapp_lua_http_response(lua_State *L, miniapp_http_response_t *response) {
    lua_pushinteger(L, response->request->id);
    lua_newtable(L);
    if (response->error) {
        lua_newtable(L);
        lua_pushstring(L, response->error_code[0] ? response->error_code : "network");
        lua_setfield(L, -2, "code");
        lua_pushstring(L, response->error_message[0] ? response->error_message : "网络请求失败");
        lua_setfield(L, -2, "message");
        lua_setfield(L, -2, "error");
    } else {
        lua_pushinteger(L, response->status);
        lua_setfield(L, -2, "status");
        lua_pushstring(L, response->content_type);
        lua_setfield(L, -2, "content_type");
        lua_pushlstring(L, response->body ? response->body : "", response->body_len);
        lua_setfield(L, -2, "body");
    }
}

static void miniapp_register_function(lua_State *L, const char *table, const char *name,
                                      lua_CFunction function) {
    lua_getglobal(L, table);
    lua_pushcfunction(L, function);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

static int miniapp_lua_audio_play(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    size_t size;
    const char *url = luaL_checklstring(L, 1, &size);
    const char *host = NULL;
    if (strncmp(url, "http://", 7) == 0) {
        host = url + 7;
    }
    if (strncmp(url, "https://", 8) == 0) {
        host = url + 8;
    }
    if (!host || !*host || *host == '/' || *host == '?' || *host == '#' ||
        size > MINIAPP_AUDIO_MAX_URL_BYTES || strlen(url) != size) {
        return luaL_error(L, "audio.play requires a bounded HTTP(S) URL");
    }
    for (size_t i = 0; i < size; ++i) {
        if ((unsigned char)url[i] <= 32 || (unsigned char)url[i] == 127) {
            return luaL_error(L, "audio URL contains whitespace or control bytes");
        }
    }
    if (vm->retiring || vm->exiting) {
        lua_pushboolean(L, false);
        return 1;
    }
    bool accepted = true;
    if (vm->validating) {
        memcpy(vm->audio_url, url, size + 1);
        vm->audio_paused = false;
    } else {
        accepted = miniapp_audio_play(url);
    }
    lua_pushboolean(L, accepted);
    return 1;
}

static int miniapp_lua_audio_pause(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (!vm->retiring && !vm->exiting) {
        if (vm->validating) {
            vm->audio_paused = true;
        } else {
            miniapp_audio_pause(true);
        }
    }
    return 0;
}

static int miniapp_lua_audio_resume(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (!vm->retiring && !vm->exiting) {
        if (vm->validating) {
            vm->audio_paused = false;
        } else {
            miniapp_audio_pause(false);
        }
    }
    return 0;
}

static int miniapp_lua_audio_stop(lua_State *L) {
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (!vm->retiring && !vm->exiting) {
        if (vm->validating) {
            vm->audio_url[0] = '\0';
        } else {
            miniapp_audio_stop();
        }
    }
    return 0;
}

static const char *miniapp_vm_audio_state(const miniapp_vm_t *vm) {
    if (!vm->validating) {
        return miniapp_audio_state();
    }
    if (!vm->audio_url[0]) {
        return "idle";
    }
    return vm->audio_paused ? "paused" : "loading";
}

static int miniapp_lua_audio_get_state(lua_State *L) {
    lua_pushstring(L, miniapp_vm_audio_state(miniapp_vm_get(L)));
    return 1;
}

static void miniapp_open_safe_libraries(lua_State *L) {
    static const struct {
        const char *name;
        lua_CFunction open;
    } libs[] = {
        {LUA_GNAME, luaopen_base},        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };

    for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); ++i) {
        luaL_requiref(L, libs[i].name, libs[i].open, 1);
        lua_pop(L, 1);
    }

    const char *forbidden[] = {"collectgarbage", "dofile", "load",   "loadfile",    "print",
                               "warn",           "pcall",  "xpcall", "setmetatable"};
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        lua_pushnil(L);
        lua_setglobal(L, forbidden[i]);
    }

#if MINIAPP_HAS_SCREEN
    if (miniapp_has_screen()) {
        lua_newtable(L);
        lua_setglobal(L, "screen");
        miniapp_lua_screen_register(L, &miniapp_vm_get(L)->scene);
        miniapp_register_function(L, "screen", "present", miniapp_lua_screen_present);
    }
#endif

#if MINIAPP_HAS_LEDS
    if (miniapp_has_led()) {
        lua_newtable(L);
        lua_setglobal(L, "led");
        miniapp_register_function(L, "led", "on", miniapp_lua_led_on);
        miniapp_register_function(L, "led", "off", miniapp_lua_led_off);
        miniapp_register_function(L, "led", "blink", miniapp_lua_led_blink);
    }
#endif

#if MINIAPP_HAS_BUZZER
    if (miniapp_has_speaker()) {
        lua_newtable(L);
        lua_setglobal(L, "buzzer");
        lua_newtable(L);
        lua_setglobal(L, "synth");
        miniapp_register_function(L, "synth", "play", miniapp_lua_synth_play);
        miniapp_register_function(L, "synth", "pause", miniapp_lua_synth_pause);
        miniapp_register_function(L, "synth", "resume", miniapp_lua_synth_resume);
        miniapp_register_function(L, "buzzer", "play", miniapp_lua_buzzer_play);
    }
#endif

    lua_newtable(L);
    lua_setglobal(L, "clock");
    miniapp_register_function(L, "clock", "now", miniapp_lua_clock_now);
    miniapp_register_function(L, "clock", "localtime", miniapp_lua_clock_localtime);
    lua_newtable(L);
    lua_setglobal(L, "storage");
    miniapp_register_function(L, "storage", "load", miniapp_lua_storage_load);
    miniapp_register_function(L, "storage", "save", miniapp_lua_storage_save);
    miniapp_register_function(L, "storage", "clear", miniapp_lua_storage_clear);

    lua_newtable(L);
    lua_setglobal(L, "http");
    miniapp_register_function(L, "http", "request", miniapp_lua_http_request);
    miniapp_register_function(L, "http", "get", miniapp_lua_http_get);
    miniapp_register_function(L, "http", "cancel", miniapp_lua_http_cancel);

    miniapp_json_open(L);

    if (miniapp_has_speaker()) {
        lua_newtable(L);
        lua_setglobal(L, "tts");
        miniapp_register_function(L, "tts", "speak", miniapp_lua_tts_speak);
        miniapp_register_function(L, "tts", "cancel", miniapp_lua_tts_cancel);

        lua_newtable(L);
        lua_setglobal(L, "audio");
        miniapp_register_function(L, "audio", "play", miniapp_lua_audio_play);
        miniapp_register_function(L, "audio", "pause", miniapp_lua_audio_pause);
        miniapp_register_function(L, "audio", "resume", miniapp_lua_audio_resume);
        miniapp_register_function(L, "audio", "stop", miniapp_lua_audio_stop);
        miniapp_register_function(L, "audio", "get_state", miniapp_lua_audio_get_state);
    }

    lua_newtable(L);
#if MINIAPP_HAS_SCREEN
    lua_pushinteger(L, MINIAPP_SCREEN_WIDTH);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, MINIAPP_SCREEN_HEIGHT);
    lua_setfield(L, -2, "height");
#endif
    lua_pushinteger(L, MINIAPP_API_VERSION);
    lua_setfield(L, -2, "api_version");
    lua_setglobal(L, "app");
}

static void miniapp_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    miniapp_vm_t *vm = miniapp_vm_get(L);
    TickType_t now = xTaskGetTickCount();
    if (vm->instruction_left <= MINIAPP_HOOK_GRANULARITY) {
        luaL_error(L, "instruction budget exceeded");
        return;
    }
    if ((int32_t)(now - vm->deadline) >= 0) {
#if configGENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
        /* A radio calibration can preempt this task for hundreds of ms.
         * FreeRTOS accounts completed running slices, excluding other tasks.
         * The current slice may be incomplete; the instruction limit remains
         * the hard bound even when no context switch updates this counter. */
        TaskStatus_t info;
        vTaskGetInfo(NULL, &info, pdFALSE, eInvalid);
        configRUN_TIME_COUNTER_TYPE cpu = info.ulRunTimeCounter - vm->cpu_started;
        if (cpu < vm->cpu_limit) {
            vm->instruction_left -= MINIAPP_HOOK_GRANULARITY;
            return;
        }
#endif
        luaL_error(L, "wall-time budget exceeded");
        return;
    }
    vm->instruction_left -= MINIAPP_HOOK_GRANULARITY;
}

static void miniapp_set_deadline(miniapp_vm_t *vm, uint32_t milliseconds) {
    vm->deadline = xTaskGetTickCount() + pdMS_TO_TICKS(milliseconds);
#if configGENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    TaskStatus_t info;
    vTaskGetInfo(NULL, &info, pdFALSE, eInvalid);
    vm->cpu_started = info.ulRunTimeCounter;
    vm->cpu_limit = (configRUN_TIME_COUNTER_TYPE)milliseconds * 1000u;
#endif
}

static int miniapp_open_libraries_protected(lua_State *L) {
    miniapp_open_safe_libraries(L);
    return 0;
}

static lua_State *miniapp_vm_create(miniapp_vm_t *vm, bool validating) {
    memset(vm, 0, sizeof(*vm));
    vm->limit = MINIAPP_LUA_HEAP_LIMIT;
    vm->http_generation = ++s_http_generation;
    vm->validating = validating;
    vm->pending_led = -1;
    vm->action_ref = LUA_NOREF;
    vm->scene.background = 0;
    lua_State *L = lua_newstate(miniapp_lua_alloc, vm);
    if (!L) {
        return NULL;
    }
    *(miniapp_vm_t **)lua_getextraspace(L) = vm;
    lua_pushcfunction(L, miniapp_open_libraries_protected);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        lua_close(L);
        return NULL;
    }
    lua_sethook(L, miniapp_hook, LUA_MASKCOUNT, MINIAPP_HOOK_GRANULARITY);
    return L;
}

static int miniapp_run_chunk(lua_State *L, miniapp_vm_t *vm, const char *source, size_t source_len,
                             char *error, size_t error_size) {
    vm->instruction_left = MINIAPP_CHUNK_INSTRUCTION_LIMIT;
    miniapp_set_deadline(vm, MINIAPP_CHUNK_DEADLINE_MS);
    int rc = luaL_loadbufferx(L, source, source_len, "@cloud-miniapp", "t");
    if (rc == LUA_OK) {
        rc = lua_pcall(L, 0, 0, 0);
    }
    if (rc != LUA_OK) {
        miniapp_storage_fault(vm);
        snprintf(error, error_size, "%s",
                 lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "Lua error");
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}

typedef enum {
    MINIAPP_CALLBACK_NO_ARGS,
    MINIAPP_CALLBACK_INTEGER,
    MINIAPP_CALLBACK_BUTTON,
    MINIAPP_CALLBACK_HTTP_RESPONSE,
    MINIAPP_CALLBACK_TTS_RESULT,
    MINIAPP_CALLBACK_AUDIO_STATE,
    MINIAPP_CALLBACK_ACTION,
} miniapp_callback_kind_t;

typedef struct {
    const char *name;
    miniapp_callback_kind_t kind;
    const char *button;
    miniapp_http_response_t *http_response;
    lua_Integer argument;
    const char *text_argument;
    const char *error_code;
    json_call_t *json_result;
    const char *action;
    const cJSON *action_args;
    bool capture_actions;
} miniapp_callback_t;

static int miniapp_callback_arguments(lua_State *L, const miniapp_callback_t *call) {
    switch (call->kind) {
    case MINIAPP_CALLBACK_INTEGER:
        lua_pushinteger(L, call->argument);
        return 1;
    case MINIAPP_CALLBACK_BUTTON:
        lua_pushstring(L, call->button);
        return 1;
    case MINIAPP_CALLBACK_HTTP_RESPONSE:
        miniapp_lua_http_response(L, call->http_response);
        return 2;
    case MINIAPP_CALLBACK_TTS_RESULT:
        lua_pushinteger(L, call->argument);
        lua_newtable(L);
        lua_pushstring(L, call->text_argument);
        lua_setfield(L, -2, "status");
        if (call->error_code) {
            lua_newtable(L);
            lua_pushstring(L, call->error_code);
            lua_setfield(L, -2, "code");
            lua_pushstring(L, call->error_code);
            lua_setfield(L, -2, "message");
            lua_setfield(L, -2, "error");
        }
        return 2;
    case MINIAPP_CALLBACK_AUDIO_STATE:
        lua_pushstring(L, call->text_argument);
        return 1;
    case MINIAPP_CALLBACK_ACTION:
        lua_pushstring(L, call->action);
        push_json(L, call->action_args, 0);
        return 2;
    case MINIAPP_CALLBACK_NO_ARGS:
        return 0;
    }
    return luaL_error(L, "unknown callback argument kind");
}

static int miniapp_callback_protected(lua_State *L) {
    miniapp_callback_t *call = lua_touserdata(L, 1);
    miniapp_vm_t *vm = miniapp_vm_get(L);
    if (call->capture_actions) {
        lua_getglobal(L, "on_action");
        if (!lua_isfunction(L, -1)) {
            return 0;
        }
        vm->action_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (call->kind == MINIAPP_CALLBACK_ACTION) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, vm->action_ref);
    } else {
        lua_getglobal(L, call->name);
    }
    /* Optional query callbacks with an invalid type advertise no capability. */
    if (call->json_result && call->kind != MINIAPP_CALLBACK_ACTION && !lua_isfunction(L, -1)) {
        return 0;
    }
    if (lua_isnil(L, -1) && strcmp(call->name, "on_tick") != 0) {
        return 0;
    }
    if (!lua_isfunction(L, -1)) {
        return luaL_error(L, "%s must be a function", call->name);
    }

    int nargs = miniapp_callback_arguments(L, call);
    lua_call(L, nargs, call->json_result ? 1 : 0);
    if (call->json_result) {
        if (!strcmp(call->name, "get_state")) {
            /* Strict string check: lua_isstring also accepts numbers. */
            if (lua_type(L, -1) != LUA_TSTRING) {
                return 0;
            }
            size_t length;
            const char *state = lua_tolstring(L, -1, &length);
            if (length > MINIAPP_ACTION_STATE_MAX || memchr(state, 0, length)) {
                return 0;
            }
        }
        fill_json(L, -1, call->json_result->root, call->json_result, 0);
    }
    return 0;
}

static int miniapp_invoke(lua_State *L, miniapp_vm_t *vm, miniapp_callback_t *call) {
    vm->instruction_left = MINIAPP_CALLBACK_INSTRUCTION_LIMIT;
    TickType_t started = xTaskGetTickCount();
    miniapp_set_deadline(vm, MINIAPP_CALLBACK_DEADLINE_MS);
    /* Even callback lookup and string arguments may allocate. Keep them under
     * pcall so an exhausted heap cannot invoke Lua's process-wide panic. */
    lua_pushcfunction(L, miniapp_callback_protected);
    lua_pushlightuserdata(L, call);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        miniapp_storage_fault(vm);
        miniapp_set_error(lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "Lua error");
        lua_pop(L, 1);
        return -1;
    }
    uint32_t elapsed = (uint32_t)((xTaskGetTickCount() - started) * portTICK_PERIOD_MS);
    s_status.last_frame_ms = elapsed;
    if (elapsed > s_status.max_frame_ms)
        s_status.max_frame_ms = elapsed;
    return 0;
}

static int miniapp_call(lua_State *L, miniapp_vm_t *vm, const char *name, int nargs,
                        lua_Integer argument) {
    miniapp_callback_t call = {
        .name = name,
        .kind = nargs ? MINIAPP_CALLBACK_INTEGER : MINIAPP_CALLBACK_NO_ARGS,
        .argument = argument,
    };
    return miniapp_invoke(L, vm, &call);
}

static int miniapp_call_button(lua_State *L, miniapp_vm_t *vm, const char *button_id) {
    miniapp_callback_t call = {
        .name = "on_button_click", .kind = MINIAPP_CALLBACK_BUTTON, .button = button_id};
    return miniapp_invoke(L, vm, &call);
}

static int miniapp_call_http_response(lua_State *L, miniapp_vm_t *vm,
                                      miniapp_http_response_t *response) {
    miniapp_callback_t call = {.name = "on_http_response",
                               .kind = MINIAPP_CALLBACK_HTTP_RESPONSE,
                               .http_response = response};
    /* Release admission quota before Lua can chain its next request. */
    miniapp_http_remove(response->request);
    int result = miniapp_invoke(L, vm, &call);
    miniapp_http_free(response->request, response);
    return result;
}

static int miniapp_call_tts_result(lua_State *L, miniapp_vm_t *vm, uint32_t speech_id,
                                   const char *result, const char *error) {
    ESP_LOGI(TAG, "TTS result id=%u status=%s error=%s", (unsigned)speech_id, result,
             error ? error : "");
    miniapp_callback_t call = {
        .name = "on_tts_result",
        .kind = MINIAPP_CALLBACK_TTS_RESULT,
        .argument = speech_id,
        .text_argument = result,
        .error_code = error,
    };
    return miniapp_invoke(L, vm, &call);
}

static int miniapp_call_audio_state(lua_State *L, miniapp_vm_t *vm) {
    const char *state = miniapp_vm_audio_state(vm);

    if (!strcmp(vm->audio_state, state)) {
        return 0;
    }

    miniapp_callback_t call = {
        .name = "on_audio_state",
        .kind = MINIAPP_CALLBACK_AUDIO_STATE,
        .text_argument = state,
    };
    int result = miniapp_invoke(L, vm, &call);
    if (result == 0) {
        snprintf(vm->audio_state, sizeof(vm->audio_state), "%s", state);
    }
    return result;
}

static int miniapp_capture_actions(lua_State *L, miniapp_vm_t *vm) {
    json_call_t result = {.root = cJSON_CreateNull(), .limit = MINIAPP_ACTIONS_BYTES_MAX};
    if (!result.root) {
        miniapp_set_error("not enough memory for action declarations");
        return -1;
    }
    miniapp_callback_t call = {
        .name = "get_actions", .capture_actions = true, .json_result = &result};
    int rc = miniapp_invoke(L, vm, &call);
    if (rc == 0 && !result.error && miniapp_actions_valid(result.root)) {
        vm->actions = result.root;
    } else {
        cJSON_Delete(result.root);
        luaL_unref(L, LUA_REGISTRYINDEX, vm->action_ref);
        vm->action_ref = LUA_NOREF;
        vm->actions = cJSON_CreateObject();
    }
    if (!vm->actions) {
        miniapp_set_error("not enough memory for action declarations");
    }
    return vm->actions ? rc : -1;
}

/* Executed only by the Lua task, after rechecking foreground and generation. */
static int miniapp_control(lua_State *L, miniapp_vm_t *vm, miniapp_control_request_t *request) {
    if (request->action) {
        const char *error =
            miniapp_action_validate(vm->actions, request->action, request->arguments);
        if (error) {
            snprintf(request->error, sizeof(request->error), "%s", error);
            return 0;
        }
    }
    json_call_t result = {.root = cJSON_CreateNull(), .limit = MINIAPP_ACTIONS_BYTES_MAX};
    if (!result.root) {
        return 0;
    }
    miniapp_callback_t call = {
        .name = request->action ? "on_action" : "get_state",
        .kind = request->action ? MINIAPP_CALLBACK_ACTION : MINIAPP_CALLBACK_NO_ARGS,
        .action = request->action,
        .action_args = request->arguments,
        .json_result = &result,
    };
    int rc = miniapp_invoke(L, vm, &call);
    if (rc != 0) {
        snprintf(request->error, sizeof(request->error), "miniapp callback failed: %.95s",
                 s_status.last_error);
        cJSON_Delete(result.root);
        return rc;
    }
    if (request->action) {
        const cJSON *ok = cJSON_GetObjectItemCaseSensitive(result.root, "ok");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(result.root, "message");
        if (result.error || !cJSON_IsObject(result.root) || !cJSON_IsBool(ok) ||
            !cJSON_IsString(message) || strlen(message->valuestring) > MINIAPP_ACTION_STATE_MAX) {
            snprintf(request->error, sizeof(request->error),
                     "on_action must return {ok=boolean, message=string}");
            cJSON_Delete(result.root);
        } else {
            request->result = result.root;
        }
        return 0;
    }
    /* Invalid state values become null without disabling the application. */
    if (result.error) {
        cJSON_Delete(result.root);
        result.root = cJSON_CreateNull();
    }
    cJSON *description = cJSON_CreateObject();
    cJSON *app = description ? cJSON_AddObjectToObject(description, "app") : NULL;
    cJSON *actions = cJSON_Duplicate(vm->actions, true);
    miniapp_package_t package;
    taskENTER_CRITICAL(&s_lock);
    package = s_current; /* Same-version installs may refresh the name. */
    taskEXIT_CRITICAL(&s_lock);
    if (!description || !app || !actions || !result.root ||
        !cJSON_AddBoolToObject(description, "running", true) ||
        !cJSON_AddStringToObject(app, "id", package.id) ||
        !cJSON_AddStringToObject(app, "version", package.version) ||
        !cJSON_AddStringToObject(app, "name", package.name)) {
        goto failed;
    }
    if (!cJSON_AddItemToObject(description, "state", result.root)) {
        goto failed;
    }
    result.root = NULL;
    if (!cJSON_AddItemToObject(description, "actions", actions)) {
        goto failed;
    }
    request->result = description;
    return 0;
failed:
    cJSON_Delete(actions);
    cJSON_Delete(result.root);
    cJSON_Delete(description);
    return 0;
}

static void miniapp_tts_stop(miniapp_vm_t *vm) {
    uint32_t owner = 0;
    taskENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < MINIAPP_TTS_RESULT_SLOTS; ++i) {
        miniapp_tts_job_t *job = s_tts_requests[i];
        if (job && job->generation == vm->http_generation) {
            if (!job->cancelled && job->speech_id == s_tts_worker_id)
                owner = job->speech_id;
            job->cancelled = true;
            if (!job->started || job->complete) {
                s_tts_requests[i] = NULL;
                free(job);
            }
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (owner)
        miniapp_platform_tts_cancel(owner);
}

static void miniapp_runtime_stop(lua_State **state, miniapp_vm_t *vm, bool navigate_home) {
    /* Normal on_exit may persist data; replacement/fault guards remain separate. */
    vm->exiting = true;
    miniapp_audio_release();
    miniapp_http_discard(vm);
    miniapp_tts_stop(vm);
    if (*state) {
        (void)miniapp_call(*state, vm, "on_exit", 0, 0);
        miniapp_tts_stop(vm);
        lua_close(*state);
        *state = NULL;
    }
    cJSON_Delete(vm->actions);
    vm->actions = NULL;
    ++s_buzzer_generation;
    s_synth_paused = false;
    miniapp_led_off();
    miniapp_set_active(false);
    taskENTER_CRITICAL(&s_lock);
    memset(&s_current, 0, sizeof(s_current));
    taskEXIT_CRITICAL(&s_lock);
    s_status.installed = false;
    if (s_buzzer_queue) {
        miniapp_audio_clear_queue();
    }
    if (navigate_home) {
        (void)miniapp_ui_close();
    }
    ESP_LOGI(TAG, "runtime stopped");
}

/* Only the Lua task owns VM lifetime. The previous VM stays intact until the
 * candidate has loaded, executed startup callbacks and reached the UI thread. */
static int miniapp_replace(lua_State **current, miniapp_vm_t **current_vm,
                           miniapp_request_t *request) {
    miniapp_vm_t *candidate = miniapp_calloc(1, sizeof(*candidate));
    lua_State *next = candidate ? miniapp_vm_create(candidate, true) : NULL;
#if MINIAPP_HAS_SCREEN
    bool had_current = *current != NULL;
    bool ui_changed = false;
#endif
    if (!next) {
        snprintf(request->error, sizeof(request->error), "not enough memory for Lua VM");
        free(candidate);
        return -1;
    }
    snprintf(candidate->id, sizeof(candidate->id), "%s", request->package->id);
    if (miniapp_run_chunk(next, candidate, request->source, request->package->size, request->error,
                          sizeof(request->error)) != 0) {
        goto failed;
    }
    if (miniapp_call(next, candidate, "on_start", 0, 0) != 0 ||
        miniapp_call(next, candidate, "on_tick", 1, MINIAPP_TICK_MS) != 0 ||
        miniapp_capture_actions(next, candidate) != 0) {
        snprintf(request->error, sizeof(request->error), "%s", s_status.last_error);
        goto failed;
    }
    if (request->reopening && !miniapp_platform_can_reopen()) {
        snprintf(request->error, sizeof(request->error), "device left the desktop");
        goto failed;
    }
#if MINIAPP_HAS_SCREEN
    if (miniapp_has_screen() && !candidate->present_count) {
        snprintf(request->error, sizeof(request->error), "startup must present a frame");
        goto failed;
    }
    ui_changed = miniapp_has_screen();
    if (miniapp_has_screen() && (miniapp_ui_present(&candidate->presented_scene) != 0 ||
                                 miniapp_ui_open() != 0 || miniapp_ui_wait_frame(2000) != 0)) {
        snprintf(request->error, sizeof(request->error), "failed to show miniapp frame");
        goto failed;
    }
#endif
    if (!request->reopening) {
        const char *error = miniapp_store_save(request->package, request->source);
        if (error) {
            snprintf(request->error, sizeof(request->error), "%s", error);
            goto failed;
        }
    }
    /* The candidate is now usable. Do not run old callbacks with device side
     * effects: they must not clear the new scene or enqueue old sounds. */
    miniapp_audio_release();
    if (*current) {
        miniapp_tts_stop(*current_vm);
        (*current_vm)->validating = true;
        (*current_vm)->retiring = true;
        (void)miniapp_call(*current, *current_vm, "on_exit", 0, 0);
        miniapp_tts_stop(*current_vm);
        miniapp_http_discard(*current_vm);
        miniapp_audio_clear_pending(*current_vm);
        lua_close(*current);
        cJSON_Delete((*current_vm)->actions);
        free(*current_vm);
    }
    *current = next;
    *current_vm = candidate;
    candidate->validating = false;
    miniapp_http_activate(candidate);
    if (candidate->storage_pending) {
        const char *error =
            candidate->storage_pending == 2
                ? miniapp_storage_clear(candidate->id)
                : miniapp_storage_save(candidate->id, candidate->storage_data,
                                       candidate->storage_size, candidate->storage_ttl);
        if (error)
            ESP_LOGW(TAG, "startup app data not saved: %s", error);
        candidate->storage_pending = 0;
    }
    ++s_buzzer_generation;
    s_synth_paused = candidate->synth_paused;
    if (s_buzzer_queue) {
        miniapp_audio_clear_queue();
    }
    miniapp_led_off();
    taskENTER_CRITICAL(&s_lock);
    s_current = *request->package;
    taskEXIT_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.installed = true;
    s_status.source_size = request->package->size;
    miniapp_set_active(true);
    miniapp_tts_activate(candidate);
    if (candidate->pending_led == 1)
        miniapp_led_on();
    if (candidate->pending_led == 2)
        miniapp_led_blink(candidate->led_on_ms, candidate->led_off_ms);
    for (unsigned i = 0; i < candidate->buzz_count; ++i) {
        candidate->buzz[i].generation = s_buzzer_generation;
        miniapp_buzzer_enqueue(&candidate->buzz[i]);
    }
    candidate->buzz_count = 0; /* Ownership transferred to queue/worker. */
    if (candidate->audio_url[0]) {
        if (!miniapp_audio_play(candidate->audio_url)) {
            ESP_LOGW(TAG, "failed to start miniapp audio worker");
        }
        miniapp_audio_pause(candidate->audio_paused);
    }
    return 0;
failed:
#if MINIAPP_HAS_SCREEN
    if (ui_changed) {
        if (had_current) {
            (void)miniapp_ui_present(&(*current_vm)->presented_scene);
            (void)miniapp_ui_wait_frame(2000);
        } else {
            (void)miniapp_ui_close();
        }
    }
#endif
    miniapp_tts_stop(candidate);
    miniapp_http_discard(candidate);
    miniapp_audio_clear_pending(candidate);
    cJSON_Delete(candidate->actions);
    lua_close(next);
    free(candidate);
    return -1;
}

static void miniapp_stop_current(lua_State **state, miniapp_vm_t **vm) {
    miniapp_runtime_stop(state, *vm, true);
    free(*vm);
    *vm = NULL;
}

static void miniapp_task(void *argument) {
    (void)argument;
    lua_State *L = NULL;
    miniapp_vm_t *vm = NULL;
    TickType_t next_tick = xTaskGetTickCount();
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = s_active ? ((int32_t)(next_tick - now) > 0 ? next_tick - now : 0)
                                   : pdMS_TO_TICKS(MINIAPP_TICK_MS);
        miniapp_event_t event;
        if (xQueueReceive(s_event_queue, &event, wait) == pdTRUE) {
            if (event.type == MINIAPP_EVENT_REOPEN) {
                if (!L && event.generation == s_buzzer_generation && miniapp_platform_can_reopen()) {
                    miniapp_package_t package;
                    char *source = NULL;
                    const char *error = miniapp_store_load(&package, &source);
                    if (!error) {
                        miniapp_request_t request = {
                            .package = &package, .source = source, .reopening = true};
                        if (miniapp_replace(&L, &vm, &request) != 0)
                            ESP_LOGW(TAG, "failed to reopen miniapp: %s", request.error);
                    } else {
                        ESP_LOGW(TAG, "failed to load miniapp: %s", error);
                    }
                    free(source);
                }
                miniapp_install_end();
                next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(MINIAPP_TICK_MS);
            } else if (event.type == MINIAPP_EVENT_INSTALL) {
                miniapp_request_t *request = event.request;
                if (event.generation != s_buzzer_generation || miniapp_platform_alarm()) {
                    snprintf(request->error, sizeof(request->error),
                             "application changed or alarm is ringing");
                    request->result = -1;
                } else {
                    request->result = miniapp_replace(&L, &vm, request);
                }
                next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(MINIAPP_TICK_MS);
                xSemaphoreGive(request->done);
                /* request belongs to the waiting caller; never touch it again. */
            } else if (event.type == MINIAPP_EVENT_DESCRIBE || event.type == MINIAPP_EVENT_ACTION) {
                miniapp_control_request_t *request = event.request;
                if (!L || !s_active || event.generation != s_buzzer_generation ||
                    miniapp_platform_alarm()) {
                    if (event.type == MINIAPP_EVENT_DESCRIBE) {
                        request->result = cJSON_CreateObject();
                        if (request->result &&
                            !cJSON_AddBoolToObject(request->result, "running", false)) {
                            cJSON_Delete(request->result);
                            request->result = NULL;
                        }
                    } else
                        snprintf(request->error, sizeof(request->error),
                                 "miniapp is not in foreground or application changed");
                } else if (miniapp_control(L, vm, request) != 0) {
                    miniapp_stop_current(&L, &vm);
                }
                xSemaphoreGive(request->done);
            } else if (event.generation != s_buzzer_generation) {
                /* Input queued for a previous instance cannot affect its replacement. */
                continue;
            } else if (event.type == MINIAPP_EVENT_CLICK && L && !miniapp_platform_alarm()) {
                if (miniapp_call_button(L, vm, event.button_id) != 0) {
                    miniapp_stop_current(&L, &vm);
                }
            } else if (event.type == MINIAPP_EVENT_EXIT && L) {
                miniapp_stop_current(&L, &vm);
            }
        }
        /* The audio worker can be blocked in synchronous network preparation. */
        miniapp_audio_poll();
        if (L && vm && !miniapp_platform_alarm() && miniapp_call_audio_state(L, vm) != 0) {
            miniapp_stop_current(&L, &vm);
        }
        miniapp_http_response_t *response;
        for (unsigned i = 0;
             i < MINIAPP_HTTP_MAX_REQUESTS &&
             (response = miniapp_http_take_result(vm, miniapp_platform_alarm())) != NULL;
             ++i) {
            if (L && vm && response->request->generation == vm->http_generation &&
                !miniapp_http_is_cancelled(response->request)) {
                if (miniapp_call_http_response(L, vm, response) != 0) {
                    miniapp_stop_current(&L, &vm);
                }
            } else
                miniapp_http_free(response->request, response);
        }
        miniapp_tts_job_t *tts_result;
        /* A callback may immediately submit another busy request. Bound each
         * drain so that callbacks cannot starve ticks or the exit event. */
        for (unsigned i = 0;
             i < MINIAPP_TTS_RESULT_SLOTS &&
             (tts_result = miniapp_tts_take_result(vm, miniapp_platform_alarm())) != NULL;
             ++i) {
            if (L && vm && !tts_result->cancelled && tts_result->generation == vm->http_generation &&
                miniapp_call_tts_result(L, vm, tts_result->speech_id, tts_result->result,
                                        tts_result->error) != 0) {
                miniapp_stop_current(&L, &vm);
            }
            free(tts_result);
        }
        /* Events must not starve ticks when the input queue remains busy. */
        now = xTaskGetTickCount();
        if (L && (int32_t)(now - next_tick) >= 0) {
            if (miniapp_platform_alarm()) {
                next_tick = now + pdMS_TO_TICKS(MINIAPP_TICK_MS);
                continue;
            }
            if (miniapp_call(L, vm, "on_tick", 1, MINIAPP_TICK_MS) != 0) {
                miniapp_stop_current(&L, &vm);
            } else {
                s_status.frames++;
            }
            next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(MINIAPP_TICK_MS);
        }
    }
}

int miniapp_init(void) {
    if (!heap_caps_get_total_size(MALLOC_CAP_SPIRAM))
        return -1;
    taskENTER_CRITICAL(&s_lock);
    if (s_initializing || s_initialized) {
        int result = s_initialized ? 0 : -1;
        taskEXIT_CRITICAL(&s_lock);
        return result;
    }
    s_initializing = true;
    taskEXIT_CRITICAL(&s_lock);
    s_event_queue = xQueueCreate(8, sizeof(miniapp_event_t));
    s_http_queue = xQueueCreate(MINIAPP_HTTP_MAX_REQUESTS, sizeof(miniapp_http_request_t *));
    s_http_lock = xSemaphoreCreateMutex();
#if MINIAPP_HAS_BUZZER
    s_buzzer_queue = xQueueCreate(MINIAPP_SYNTH_QUEUE_DEPTH, sizeof(miniapp_buzzer_command_t));
    if (!s_buzzer_queue) {
        goto failed;
    }
#endif
    if (!s_event_queue || !s_http_queue || !s_http_lock) {
        goto failed;
    }
    /* Networking and PCM workers do not write flash. Keep their stacks in
     * PSRAM; only the Lua task needs an internal stack for NVS operations. */
    if (xTaskCreateWithCaps(miniapp_http_task, "miniapp.http", 6144, NULL, 4, &s_http_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        goto failed;
    }

#if MINIAPP_HAS_BUZZER
    if (xTaskCreateWithCaps(miniapp_buzzer_task, "miniapp.buzz", 6144, NULL, 4, &s_buzzer_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        goto failed;
    }
#endif
    if (xTaskCreate(miniapp_task, "miniapp.lua", 8192, NULL, 5, &s_task) != pdPASS) {
        goto failed;
    }
    /* The navigation registry has no remove operation. Register the UI only
     * after every task is ready so a later startup failure cannot strand the
     * page registration or its scene buffers. */
    if (miniapp_ui_init() != 0) {
        goto failed;
    }
    taskENTER_CRITICAL(&s_lock);
    s_initialized = true;
    s_initializing = false;
    taskEXIT_CRITICAL(&s_lock);
    return 0;
failed:
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_http_task) {
        vTaskDeleteWithCaps(s_http_task);
        s_http_task = NULL;
    }
    if (s_http_lock) {
        vSemaphoreDelete(s_http_lock);
        s_http_lock = NULL;
    }
    if (s_http_queue) {
        vQueueDelete(s_http_queue);
        s_http_queue = NULL;
    }
    if (s_buzzer_task) {
        vTaskDeleteWithCaps(s_buzzer_task);
        s_buzzer_task = NULL;
    }
    if (s_buzzer_queue) {
        vQueueDelete(s_buzzer_queue);
        s_buzzer_queue = NULL;
    }
    if (s_event_queue) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
    taskENTER_CRITICAL(&s_lock);
    s_initializing = false;
    taskEXIT_CRITICAL(&s_lock);
    return -1;
}

bool miniapp_is_active(void) {
    return s_active;
}
bool miniapp_ready(void) {
    taskENTER_CRITICAL(&s_lock);
    bool ready = s_initialized;
    taskEXIT_CRITICAL(&s_lock);
    return ready;
}

const char *miniapp_mode(void) {
    return miniapp_is_active() ? "miniapp" : NULL;
}

bool miniapp_identity(char *id, size_t id_size, char *version, size_t version_size) {
    bool active;

    if (!id || !id_size || !version || !version_size)
        return false;
    id[0] = '\0';
    version[0] = '\0';
    taskENTER_CRITICAL(&s_lock);
    active = s_active && s_current.id[0] && s_current.version[0];
    if (active) {
        snprintf(id, id_size, "%s", s_current.id);
        snprintf(version, version_size, "%s", s_current.version);
    }
    taskEXIT_CRITICAL(&s_lock);
    return active;
}

bool miniapp_install_begin(void) {
    bool accepted;
    taskENTER_CRITICAL(&s_lock);
    accepted = s_initialized && !s_install_busy;
    if (accepted) {
        s_install_busy = true;
        s_install_generation = s_buzzer_generation;
    }
    taskEXIT_CRITICAL(&s_lock);
    return accepted;
}

void miniapp_install_end(void) {
    taskENTER_CRITICAL(&s_lock);
    s_install_busy = false;
    taskEXIT_CRITICAL(&s_lock);
}

int miniapp_reopen(void) {
    if (miniapp_is_active() || !miniapp_platform_can_reopen() || !miniapp_store_has_app() ||
        miniapp_init() != 0 || !miniapp_install_begin()) return -1;
    miniapp_event_t event = {.type = MINIAPP_EVENT_REOPEN, .generation = s_install_generation};
    if (xQueueSend(s_event_queue, &event, 0) == pdTRUE) return 0;
    miniapp_install_end();
    return -1;
}

bool miniapp_matches(const miniapp_package_t *package) {
    bool match;
    taskENTER_CRITICAL(&s_lock);
    match = s_active && s_current.size == package->size && strcmp(s_current.id, package->id) == 0 &&
            strcmp(s_current.version, package->version) == 0 &&
            strcmp(s_current.hash, package->hash) == 0;
    if (match)
        snprintf(s_current.name, sizeof(s_current.name), "%s", package->name);
    taskEXIT_CRITICAL(&s_lock);
    return match;
}

int miniapp_install(const miniapp_package_t *package, const char *source, char *error,
                    size_t error_size) {
    if (!s_task || !package || !source || !error || !error_size || package->size == 0 ||
        package->size > MINIAPP_SOURCE_MAX || memchr(source, '\0', package->size)) {
        if (error && error_size) {
            snprintf(error, error_size, "invalid Lua source or runtime unavailable");
        }
        return -1;
    }
    miniapp_request_t request = {.package = package, .source = source, .result = -1};
    request.done = xSemaphoreCreateBinary();
    if (!request.done) {
        snprintf(error, error_size, "not enough memory for install request");
        return -1;
    }
    miniapp_event_t event = {
        .type = MINIAPP_EVENT_INSTALL, .request = &request, .generation = s_install_generation};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        vSemaphoreDelete(request.done);
        snprintf(error, error_size, "miniapp queue is busy");
        return -1;
    }
    /* The MCP worker owns these buffers until the bounded Lua/UI operations
     * finish. A caller timeout must never free an in-flight request. */
    xSemaphoreTake(request.done, portMAX_DELAY);
    vSemaphoreDelete(request.done);
    snprintf(error, error_size, "%s", request.error);
    return request.result;
}

int miniapp_button_click(const char *button_id) {
    if (!MINIAPP_HAS_BUTTONS)
        return 0;
    if (!s_active || !button_id || !miniapp_button_known(button_id)) {
        return -1;
    }
    miniapp_event_t event = {.type = MINIAPP_EVENT_CLICK, .generation = s_buzzer_generation};
    snprintf(event.button_id, sizeof(event.button_id), "%s", button_id);
    return xQueueSend(s_event_queue, &event, 0) == pdTRUE ? 0 : -1;
}

static cJSON *miniapp_control_request(const char *action, const cJSON *arguments, char *error,
                                      size_t error_size) {
    if (!error || !error_size) {
        return NULL;
    }
    error[0] = 0;
    if (!s_task || xTaskGetCurrentTaskHandle() == s_task) {
        snprintf(error, error_size, "miniapp runtime unavailable");
        return NULL;
    }
    miniapp_control_request_t request = {.action = action, .arguments = arguments};
    request.done = xSemaphoreCreateBinary();
    if (!request.done) {
        return NULL;
    }
    miniapp_event_t event = {
        .type = action ? MINIAPP_EVENT_ACTION : MINIAPP_EVENT_DESCRIBE,
        .request = &request,
        .generation = s_buzzer_generation,
    };
    /* Do not let an action received during an alarm run after it ends. */
    if (action && (!s_active || miniapp_platform_alarm())) {
        snprintf(error, error_size, "miniapp is not in foreground");
    } else if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        snprintf(error, error_size, "miniapp queue is busy");
    } else {
        /* Keep caller buffers alive until the bounded callback has finished. */
        xSemaphoreTake(request.done, portMAX_DELAY);
        snprintf(error, error_size, "%s", request.error);
    }
    vSemaphoreDelete(request.done);
    return request.result;
}

cJSON *miniapp_describe(char *error, size_t error_size) {
    return miniapp_control_request(NULL, NULL, error, error_size);
}

cJSON *miniapp_action(const char *action, const cJSON *arguments, char *error, size_t error_size) {
    if (!action) {
        if (error && error_size) {
            snprintf(error, error_size, "invalid action name");
        }
        return NULL;
    }
    return miniapp_control_request(action, arguments, error, error_size);
}

int miniapp_exit(void) {
    if (!s_active) {
        return -1;
    }
    miniapp_event_t event = {.type = MINIAPP_EVENT_EXIT, .generation = s_buzzer_generation};
    return xQueueSendToFront(s_event_queue, &event, 0) == pdTRUE ? 0 : -1;
}

void miniapp_runtime_ui_closed(void) {
    if (s_active) {
        (void)miniapp_exit();
    }
}
