#include "miniapp_synth.h"
#include <string.h>
#define VOICE_PEAK (MINIAPP_SYNTH_AMPLITUDE / 2u)

void miniapp_synth_init(miniapp_synth_t *synth) {
    memset(synth, 0, sizeof(*synth));
}

static uint32_t noise_next(uint32_t value) {
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
}

void miniapp_synth_seek(miniapp_synth_t *synth, const miniapp_score_t *score, uint32_t sample) {
    miniapp_synth_init(synth);
    synth->cursor = sample < score->samples ? sample : score->samples;
    while (synth->next < score->count && score->notes[synth->next].at < synth->cursor) {
        const miniapp_note_t *note = &score->notes[synth->next++];
        miniapp_oscillator_t *osc = &synth->voices[note->voice];
        osc->note = *note;
        osc->age = synth->cursor - note->at;
        if (osc->age > note->duration) {
            osc->age = note->duration;
        }
        osc->phase = (osc->age * note->hz) % MINIAPP_SYNTH_RATE;
        osc->noise = 0x12345678u + synth->next;
        if (note->wave == SYNTH_NOISE && osc->age) {
            unsigned updates = (osc->age - 1u) * note->hz / MINIAPP_SYNTH_RATE + 1u;
            while (updates--) {
                osc->noise = noise_next(osc->noise);
            }
        }
    }
}

size_t miniapp_synth_render(miniapp_synth_t *synth, const miniapp_score_t *score, int16_t *pcm,
                            size_t capacity) {
    size_t n = 0;
    for (; n < capacity && synth->cursor < score->samples; ++n, ++synth->cursor) {
        while (synth->next < score->count && score->notes[synth->next].at <= synth->cursor) {
            const miniapp_note_t *note = &score->notes[synth->next++];
            miniapp_oscillator_t *osc = &synth->voices[note->voice];
            osc->note = *note;
            osc->age = osc->phase = 0;
            osc->noise = 0x12345678u + synth->next;
        }
        int32_t mix = 0;
        for (unsigned v = 0; v < MINIAPP_SYNTH_VOICES; ++v) {
            miniapp_oscillator_t *osc = &synth->voices[v];
            if (osc->age >= osc->note.duration) {
                continue;
            }
            int32_t wave;
            if (osc->note.wave == SYNTH_NOISE) {
                /* xorshift noise, held for one oscillator period. */
                if (osc->phase < osc->note.hz) {
                    osc->noise = noise_next(osc->noise);
                }
                wave = (osc->noise & 1u) ? (int32_t)VOICE_PEAK : -(int32_t)VOICE_PEAK;
            } else if (osc->note.wave == SYNTH_TRIANGLE) {
                int32_t ramp = (int32_t)(osc->phase * (4u * VOICE_PEAK) / MINIAPP_SYNTH_RATE);
                wave = ramp < 2 * (int32_t)VOICE_PEAK ? ramp - (int32_t)VOICE_PEAK
                                                      : 3 * (int32_t)VOICE_PEAK - ramp;
            } else {
                wave = osc->phase < MINIAPP_SYNTH_RATE / 2 ? (int32_t)VOICE_PEAK
                                                           : -(int32_t)VOICE_PEAK;
            }
            /* 5 ms attack, 15 ms release; master limiter below handles overlap. */
            uint32_t remaining = osc->note.duration - osc->age;
            int32_t envelope = 240;
            if (osc->age < 80) {
                envelope = (int32_t)osc->age * 3;
            }
            if (remaining < (uint32_t)envelope) {
                envelope = (int32_t)remaining;
            }
            mix += wave * osc->note.volume * envelope / 24000;
            osc->phase = (osc->phase + osc->note.hz) % MINIAPP_SYNTH_RATE;
            ++osc->age;
        }
        /* Soft knee: retain quiet passages, compress coincident peaks without
         * hard clipping. The output remains strictly below the synth peak. */
        int32_t magnitude = mix < 0 ? -mix : mix;
        const int32_t knee = MINIAPP_SYNTH_AMPLITUDE * 2 / 3;
        const int32_t headroom = MINIAPP_SYNTH_AMPLITUDE - knee;
        if (magnitude > knee) {
            int32_t excess = magnitude - knee;
            magnitude = knee + excess * headroom / (excess + headroom);
            mix = mix < 0 ? -magnitude : magnitude;
        }
        pcm[n] = (int16_t)mix;
    }
    return n;
}
