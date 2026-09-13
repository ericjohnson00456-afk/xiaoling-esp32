#ifndef BOARD_DISPLAY_HARDWARE_H
#define BOARD_DISPLAY_HARDWARE_H

#include <stdbool.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

enum class DisplayHardwareType {
    None,
    Monochrome,
    Spi,
    Rgb,
    Mipi,
};

enum class DisplayPixelFormat {
    Rgb565,
    Rgb888,
    Gray1,
};

// Hardware-only display description.  Board code publishes initialized panel
// handles and geometry; the UI layer creates the concrete Display object.
struct DisplayHardwareDescriptor {
    DisplayHardwareType type = DisplayHardwareType::None;
    DisplayPixelFormat pixel_format = DisplayPixelFormat::Rgb565;
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    int width = 0;
    int height = 0;
    int offset_x = 0;
    int offset_y = 0;
    bool mirror_x = false;
    bool mirror_y = false;
    bool swap_xy = false;
};

#endif  // BOARD_DISPLAY_HARDWARE_H
