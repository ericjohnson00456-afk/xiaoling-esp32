#pragma once
#include <stdbool.h>
#define MINIAPP_AUDIO_MAX_URL_BYTES 511u
#ifdef __cplusplus
extern "C" {
#endif
bool miniapp_audio_play(const char *url);
void miniapp_audio_pause(bool paused);
void miniapp_audio_stop(void);
void miniapp_audio_poll(void);
void miniapp_audio_release(void);
const char *miniapp_audio_state(void);
#ifdef __cplusplus
}
#endif
