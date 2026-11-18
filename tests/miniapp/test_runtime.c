#include "runtime_host.h"
/* Exercise production entry points, candidate replacement and result delivery
 * with the real embedded Lua VM. Only hardware/RTOS boundaries are stubbed. */
#include "../../main/miniapp/miniapp_runtime.c"

static bool busy;
static unsigned claims, cancellations, clears;
static uint32_t claimed_id;
static char saved[128] = "{\"progress\":42}";
static bool source_save_fails, desktop = true;
static unsigned source_saves;
bool miniapp_store_supported(void) { return true; }
bool miniapp_store_has_app(void) { return source_saves != 0; }
const char *miniapp_store_load(miniapp_package_t *p, char **source) { return "empty"; }
const char *miniapp_store_save(const miniapp_package_t *p, const char *source) {
    if (source_save_fails) return "flash write failed";
    ++source_saves;
    return NULL;
}
bool miniapp_store_clear(void) { return true; }
bool miniapp_platform_can_reopen(void) { return desktop; }
bool miniapp_platform_busy(void) { return busy; }
bool miniapp_platform_alarm(void) { return false; }
void miniapp_platform_mode_changed(bool active) {}
void miniapp_platform_interrupt(void) {}
bool miniapp_platform_tts_claim(uint32_t id) { ++claims; claimed_id = id; return true; }
void miniapp_platform_tts_cancel(uint32_t id) { ++cancellations; }
void miniapp_platform_tts_release(uint32_t id) {}
void miniapp_platform_tts(const char *s, uint32_t id, const char **result, const char **error) {
    *result = "completed"; *error = NULL;
}
void miniapp_platform_sound(const miniapp_score_t *s, bool loop, uint16_t hz, uint16_t ms,
                           uint32_t generation, const volatile uint32_t *current,
                           const volatile bool *paused) {}
bool miniapp_time_valid(void) { return true; }
int miniapp_get_time(struct timeval *tv) { tv->tv_sec = 1700000000; return 0; }
int miniapp_localtime(struct tm *tm) { memset(tm, 0, sizeof(*tm)); return 0; }
int miniapp_utc_offset(void) { return 0; }
void miniapp_led_on(void) {}
void miniapp_led_off(void) {}
void miniapp_led_blink(uint32_t on, uint32_t off) {}
bool miniapp_has_screen(void) { return false; }
bool miniapp_has_speaker(void) { return true; }
bool miniapp_has_led(void) { return false; }
bool miniapp_button_known(const char *id) { return true; }
int miniapp_screen_width(void) { return 320; }
int miniapp_screen_height(void) { return 240; }
bool miniapp_audio_play(const char *s) { return true; }
void miniapp_audio_pause(bool p) {}
void miniapp_audio_stop(void) {}
void miniapp_audio_poll(void) {}
void miniapp_audio_release(void) {}
const char *miniapp_audio_state(void) { return "idle"; }
int miniapp_ui_init(void) { return 0; }
int miniapp_ui_open(void) { return 0; }
int miniapp_ui_close(void) { return 0; }
int miniapp_ui_present(const miniapp_scene_t *s) { return 0; }
int miniapp_ui_wait_frame(uint32_t timeout) { return 0; }
int miniapp_storage_load(const char *id, char *data, size_t *size) {
    if (!saved[0]) return 0;
    *size = strlen(saved); strcpy(data, saved); return 1;
}
const char *miniapp_storage_save(const char *id, const char *data, size_t size, uint32_t ttl) {
    assert(size < sizeof(saved)); memcpy(saved, data, size); saved[size] = 0; return NULL;
}
const char *miniapp_storage_clear(const char *id) { ++clears; saved[0] = 0; return NULL; }

static void run(lua_State *L, const char *source) {
    int rc = luaL_dostring(L, source);
    if (rc != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
    assert(rc == LUA_OK);
}
static void complete_worker(void) {
    assert(host_worker);
    void (*fn)(void *) = host_worker;
    void *arg = host_worker_arg;
    host_worker = NULL;
    fn(arg);
}
static void drain(lua_State *L, miniapp_vm_t *vm) {
    miniapp_tts_job_t *job;
    while ((job = miniapp_tts_take_result(vm, false))) {
        if (!job->cancelled && job->generation == vm->http_generation)
            assert(miniapp_call_tts_result(L, vm, job->speech_id, job->result, job->error) == 0);
        free(job);
    }
}
static void assert_no_jobs(void) {
    for (unsigned i = 0; i < MINIAPP_TTS_RESULT_SLOTS; ++i) assert(!s_tts_requests[i]);
}
int main(void) {
    miniapp_vm_t vm;
    lua_State *L = miniapp_vm_create(&vm, false);
    assert(L);
    run(L, "results={} function on_tts_result(id,r) results[id]=r end");
    const char *bad[] = {"nil", "123", "''", "string.rep('a',513)", "'a'..string.char(0)..'b'"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        char source[256];
        snprintf(source, sizeof(source), "id=tts.speak(%s); assert(type(id)=='number'); assert(results[id]==nil)", bad[i]);
        run(L, source);
        assert(claims == 0);
        drain(L, &vm);
        run(L, "assert(results[id].status=='failed'); assert(results[id].error.code)");
        assert(!vm.faulted);
    }
    busy = true;
    run(L, "id=tts.speak('hello')");
    drain(L, &vm);
    run(L, "assert(results[id].error.code=='busy')");
    busy = false;
    host_task_fail = true;
    run(L, "id=tts.speak('hello')");
    drain(L, &vm);
    run(L, "assert(results[id].error.code=='unavailable')");
    host_task_fail = false;
    run(L, "id=tts.speak('hello')");
    assert(host_worker && claimed_id);
    complete_worker();
    /* Completion belongs to this VM and remains pending while frozen. */
    assert(!miniapp_tts_take_result(&vm, true));
    run(L, "assert(not tts.cancel(id))");
    drain(L, &vm);
    run(L, "assert(results[id].status=='completed')");
    run(L, "id=tts.speak('cancel'); assert(tts.cancel(id)); assert(not tts.cancel(id))");
    complete_worker();
    drain(L, &vm);
    run(L, "assert(results[id]==nil)");
    lua_close(L);
    assert_no_jobs();

    /* Failed same-ID replacement preserves the old VM, speech and save. */
    miniapp_vm_t *current_vm = NULL;
    lua_State *current = NULL;
    miniapp_package_t package = {.id="same-app"};
    const char *old = "function on_tick() end; function get_state() return 'old' end";
    package.size = strlen(old);
    miniapp_request_t request = {.package=&package, .source=old};
    assert(miniapp_replace(&current, &current_vm, &request) == 0);
    run(current, "id=tts.speak('old speech')");
    unsigned before_claims = claims, before_cancel = cancellations;
    lua_State *previous = current;
    const char *failed = "storage.load(); tts.speak('candidate'); error('startup failed'); function on_tick() end";
    package.size = strlen(failed); request.source = failed;
    assert(miniapp_replace(&current, &current_vm, &request) != 0);
    assert(current == previous && clears == 0 && saved[0]);
    assert(claims == before_claims && cancellations == before_cancel);
    run(current, "assert(get_state()=='old')");
    complete_worker(); drain(current, current_vm);

    /* Flash failure must not retire the old VM, speech or app data. */
    source_save_fails = true;
    request.source = old; package.size = strlen(old);
    unsigned before_saves = source_saves;
    assert(miniapp_replace(&current, &current_vm, &request) != 0);
    assert(current == previous && saved[0] && source_saves == before_saves);
    run(current, "assert(get_state()=='old')");
    source_save_fails = false;

    /* Reopening does not write flash again, and a stale desktop request is rejected. */
    request.reopening = true;
    desktop = false;
    assert(miniapp_replace(&current, &current_vm, &request) != 0);
    assert(current == previous && source_saves == before_saves);
    desktop = true;
    assert(miniapp_replace(&current, &current_vm, &request) == 0);
    assert(source_saves == before_saves);
    request.reopening = false;

    /* Startup requests activate only after commit; startup cancellation is silent. */
    const char *next = "results={} id=tts.speak('new speech'); gone=tts.speak('cancel me'); assert(tts.cancel(gone)); function on_tick() end; function on_tts_result(id,r) results[id]=r end";
    package.size = strlen(next); request.source = next;
    assert(miniapp_replace(&current, &current_vm, &request) == 0);
    assert(claims == before_claims + 1 && host_worker);
    complete_worker(); drain(current, current_vm);
    run(current, "assert(results[id].status=='completed'); assert(results[gone]==nil)");
    /* A retiring worker must not deliver its result into the replacement VM. */
    run(current, "id=tts.speak('retiring speech')");
    unsigned retiring_cancels = cancellations;
    const char *replacement = "results={} function on_tick() end; function on_tts_result(id,r) results[id]=r end";
    package.size = strlen(replacement); request.source = replacement;
    assert(miniapp_replace(&current, &current_vm, &request) == 0);
    assert(cancellations == retiring_cancels + 1);
    complete_worker(); drain(current, current_vm);
    run(current, "assert(next(results)==nil)");
    /* Active-instance faults still clear a save that was read, per API 3. */
    const char *fault = "storage.load(); error('runtime')";
    assert(miniapp_run_chunk(current, current_vm, fault, strlen(fault),
                             request.error, sizeof(request.error)) != 0);
    assert(clears == 1 && !saved[0]);
    miniapp_stop_current(&current, &current_vm);
    assert_no_jobs();
    puts("miniapp runtime TTS and candidate isolation passed");
    return 0;
}
