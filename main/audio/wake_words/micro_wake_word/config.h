#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct MicroWakeWordConfig {
    // 必须由模型描述 JSON 的 micro.probability_cutoff 填充。
    float probability_cutoff = 0.0f;
    size_t sliding_window_size = 5;
    uint8_t feature_step_size = 10;
    size_t tensor_arena_size = 128 * 1024;
    std::string wake_word;
};
