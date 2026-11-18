#include "miniapp_synth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    miniapp_score_t score = {.count = 3, .samples = 16000};
    score.notes[0] = (miniapp_note_t){
        .at = 0, .duration = 16000, .hz = 440, .voice = 0, .volume = 100, .wave = SYNTH_SQUARE};
    score.notes[1] = (miniapp_note_t){
        .at = 0, .duration = 16000, .hz = 660, .voice = 1, .volume = 100, .wave = SYNTH_TRIANGLE};
    score.notes[2] = (miniapp_note_t){
        .at = 0, .duration = 16000, .hz = 100, .voice = 2, .volume = 100, .wave = SYNTH_NOISE};
    miniapp_synth_t synth;
    miniapp_synth_init(&synth);
    int16_t *full = malloc(32000);
    assert(full);
    assert(miniapp_synth_render(&synth, &score, full, 16000) == 16000);
    for (unsigned i = 0; i < 16000; ++i)
        assert(abs(full[i]) <= MINIAPP_SYNTH_AMPLITUDE);
    int16_t tail[512];
    miniapp_synth_seek(&synth, &score, 8000);
    assert(miniapp_synth_render(&synth, &score, tail, 512) == 512);
    assert(!memcmp(full + 8000, tail, sizeof(tail)));
    miniapp_synth_seek(&synth, &score, 16000);
    assert(!miniapp_synth_render(&synth, &score, tail, 512));
    free(full);
    puts("miniapp synth mixing and resume passed");
}
