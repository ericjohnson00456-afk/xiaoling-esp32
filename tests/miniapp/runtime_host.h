/* Host-only RTOS/network boundary for exercising the real Lua runtime.
 * Tasks are explicitly completed by tests; no network or hardware is simulated. */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_MINIAPP 1
#define CONFIG_MINIAPP_SOURCE_MAX_BYTES 65536
#define CONFIG_MINIAPP_HEAP_MAX_BYTES 262144
#define configGENERATE_RUN_TIME_STATS 0
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
typedef int portMUX_TYPE;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
static void (*host_worker)(void *);
static void *host_worker_arg;
static bool host_task_fail;
static inline TickType_t xTaskGetTickCount(void) { return 1; }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
static inline int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                             void *arg, unsigned priority, TaskHandle_t *task) {
    if (host_task_fail) return 0;
    assert(!host_worker);
    host_worker = fn;
    host_worker_arg = arg;
    if (task) *task = (void *)1;
    return pdPASS;
}
static inline int xTaskCreateWithCaps(void (*fn)(void *), const char *name, unsigned stack,
                                     void *arg, unsigned priority, TaskHandle_t *task, int caps) {
    return xTaskCreate(fn, name, stack, arg, priority, task);
}
static inline void vTaskDelete(TaskHandle_t task) {}
static inline void vTaskDeleteWithCaps(TaskHandle_t task) {}
static inline QueueHandle_t xQueueCreate(unsigned count, size_t size) { return (void *)1; }
static inline int xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) { return pdTRUE; }
static inline int xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t wait) { return pdTRUE; }
static inline int xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) { return pdFALSE; }
static inline void vQueueDelete(QueueHandle_t q) {}
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)1; }
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) { return pdTRUE; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { return pdTRUE; }
static inline void vSemaphoreDelete(SemaphoreHandle_t s) {}
static inline void *heap_caps_malloc(size_t n, int caps) { return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t size, int caps) { return calloc(n, size); }
static inline void *heap_caps_realloc(void *p, size_t n, int caps) { return realloc(p, n); }
static inline size_t heap_caps_get_total_size(int caps) { return 8 * 1024 * 1024; }
typedef int esp_err_t;
typedef int esp_http_client_method_t;
typedef void *esp_http_client_handle_t;
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
#define HTTP_EVENT_ON_HEADER 1
typedef struct {
    void *user_data;
    int event_id;
    char *header_key, *header_value;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    int method;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    unsigned timeout_ms;
    bool disable_auto_redirect;
} esp_http_client_config_t;
static inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) { abort(); }
static inline int esp_http_client_set_header(void *c, const char *k, const char *v) { abort(); }
static inline int esp_http_client_open(void *c, size_t n) { abort(); }
static inline int esp_http_client_write(void *c, const char *s, size_t n) { abort(); }
static inline int64_t esp_http_client_fetch_headers(void *c) { abort(); }
static inline int esp_http_client_get_status_code(void *c) { abort(); }
static inline int esp_http_client_set_timeout_ms(void *c, unsigned n) { abort(); }
static inline int esp_http_client_read(void *c, char *s, size_t n) { abort(); }
static inline bool esp_http_client_is_complete_data_received(void *c) { abort(); }
static inline int esp_http_client_close(void *c) { abort(); }
static inline int esp_http_client_cleanup(void *c) { abort(); }
