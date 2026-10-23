#include "ui/utils/image/rgb565_pixels.h"

#include <array>
#include <cassert>
#include <cstdio>

int main() {
    // Odd width and row padding catch accidental use of tightly packed BGRA.
    // Transparent colored pixels must become black, rather than exposing their RGB.
    const std::array<uint8_t, 32> source = {
        0, 0, 255, 255,    0, 255, 0, 255,   255, 0, 0, 255,  99, 99, 99, 99,
        255, 255, 255, 0,  255, 255, 255, 128, 255, 255, 255, 255, 99, 99, 99, 99,
    };
    const std::array<uint8_t, 12> expected = {
        0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00,
        0x00, 0x00, 0x10, 0x84, 0xff, 0xff,
    };
    std::array<uint8_t, 14> output;
    output.fill(0xa5);
    BgraOnBlackToRgb565(source.data(), 16, 3, 2, output.data() + 1);
    assert(output.front() == 0xa5 && output.back() == 0xa5);
    for (size_t i = 0; i < expected.size(); ++i) {
        assert(output[i + 1] == expected[i]);
    }
    std::puts("RGB565: channel order, alpha-on-black, row padding and buffer boundaries passed");
}
