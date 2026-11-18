#ifdef __cplusplus
extern "C" {
#endif
#ifndef MINIAPP_SYNTH_H
#define MINIAPP_SYNTH_H
#include <stdint.h>
#include <stddef.h>
#define MINIAPP_SYNTH_RATE 16000u
#define MINIAPP_SYNTH_VOICES 4u
#define MINIAPP_SYNTH_NOTES 256u
/* Source peaks before applying the system volume. */
#define MINIAPP_TONE_AMPLITUDE 6000
#define MINIAPP_SYNTH_AMPLITUDE 12000

typedef enum { SYNTH_SQUARE, SYNTH_TRIANGLE, SYNTH_NOISE } miniapp_wave_t;
typedef struct {
    uint32_t at; /* sample clock */
    uint32_t duration;
    uint16_t hz;
    uint8_t voice, wave, volume;
} miniapp_note_t;
typedef struct {
    uint32_t count, samples;
    miniapp_note_t notes[MINIAPP_SYNTH_NOTES];
} miniapp_score_t;
typedef struct {
    miniapp_note_t note;
    uint32_t phase, age, noise;
} miniapp_oscillator_t;
typedef struct {
    uint32_t cursor, next;
    miniapp_oscillator_t voices[MINIAPP_SYNTH_VOICES];
} miniapp_synth_t;
void miniapp_synth_init(miniapp_synth_t *synth);
void miniapp_synth_seek(miniapp_synth_t *synth, const miniapp_score_t *score, uint32_t sample);
size_t miniapp_synth_render(miniapp_synth_t *synth, const miniapp_score_t *score, int16_t *pcm,
                            size_t capacity);
#endif

#ifdef __cplusplus
}
#endif
