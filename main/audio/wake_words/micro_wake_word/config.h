#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifndef MICRO_WAKE_WORD_MODEL_PROBABILITY_CUTOFF
#define MICRO_WAKE_WORD_MODEL_PROBABILITY_CUTOFF 90
#endif

struct MicroWakeWordConfig {
    float probability_cutoff =
        static_cast<float>(MICRO_WAKE_WORD_MODEL_PROBABILITY_CUTOFF) / 100.0f;
    size_t sliding_window_size = 5;
    uint8_t feature_step_size = 10;
    size_t tensor_arena_size = 128 * 1024;
    std::string wake_word;
};
