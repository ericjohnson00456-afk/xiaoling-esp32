#pragma once

#include <cstddef>
#include <cstdint>

// LVGL's decoded ARGB8888 buffer stores bytes in BGRA order. Composite its
// straight alpha onto black once, then keep native little-endian RGB565 pixels.
// The source may have row padding; the destination is tightly packed.
inline void BgraOnBlackToRgb565(const uint8_t* source, size_t source_stride,
                              size_t width, size_t height, uint8_t* destination) {
    for (size_t y = 0; y < height; ++y) {
        const uint8_t* row = source + y * source_stride;
        for (size_t x = 0; x < width; ++x) {
            const uint8_t* pixel = row + x * 4;
            const uint32_t alpha = pixel[3];
            const uint32_t red = (pixel[2] * alpha + 127) / 255;
            const uint32_t green = (pixel[1] * alpha + 127) / 255;
            const uint32_t blue = (pixel[0] * alpha + 127) / 255;
            const uint16_t rgb565 = static_cast<uint16_t>(
                ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
            *destination++ = static_cast<uint8_t>(rgb565);
            *destination++ = static_cast<uint8_t>(rgb565 >> 8);
        }
    }
}
