#include "display_factory.h"

#include "ui/display/display.h"
#include "ui/display/panels/lcd_display.h"
#include "ui/display/panels/oled_display.h"

Display* CreateDisplay(const DisplayHardwareDescriptor& hardware) {
    switch (hardware.type) {
        case DisplayHardwareType::None:
            return new NoDisplay();
        case DisplayHardwareType::Monochrome:
            return new OledDisplay(hardware.panel_io, hardware.panel,
                                   hardware.width, hardware.height,
                                   hardware.mirror_x, hardware.mirror_y);
        case DisplayHardwareType::Spi:
            return new SpiLcdDisplay(hardware.panel_io, hardware.panel,
                                     hardware.width, hardware.height,
                                     hardware.offset_x, hardware.offset_y,
                                     hardware.mirror_x, hardware.mirror_y,
                                     hardware.swap_xy);
        case DisplayHardwareType::Rgb:
            return new RgbLcdDisplay(hardware.panel_io, hardware.panel,
                                     hardware.width, hardware.height,
                                     hardware.offset_x, hardware.offset_y,
                                     hardware.mirror_x, hardware.mirror_y,
                                     hardware.swap_xy);
        case DisplayHardwareType::Mipi:
            return new MipiLcdDisplay(hardware.panel_io, hardware.panel,
                                      hardware.width, hardware.height,
                                      hardware.offset_x, hardware.offset_y,
                                      hardware.mirror_x, hardware.mirror_y,
                                      hardware.swap_xy);
    }
    return new NoDisplay();
}

Display* CreateNoDisplay() {
    return CreateDisplay(DisplayHardwareDescriptor{});
}

Display* CreateOledDisplay(esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_handle_t panel,
                           int width, int height,
                           bool mirror_x, bool mirror_y) {
    return CreateDisplay(DisplayHardwareDescriptor{
        .type = DisplayHardwareType::Monochrome,
        .pixel_format = DisplayPixelFormat::Gray1,
        .panel_io = panel_io,
        .panel = panel,
        .width = width,
        .height = height,
        .mirror_x = mirror_x,
        .mirror_y = mirror_y,
    });
}

Display* CreateSpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_panel_handle_t panel,
                             int width, int height,
                             int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y,
                             bool swap_xy) {
    return CreateDisplay(DisplayHardwareDescriptor{
        .type = DisplayHardwareType::Spi,
        .panel_io = panel_io,
        .panel = panel,
        .width = width,
        .height = height,
        .offset_x = offset_x,
        .offset_y = offset_y,
        .mirror_x = mirror_x,
        .mirror_y = mirror_y,
        .swap_xy = swap_xy,
    });
}

Display* CreateRgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_panel_handle_t panel,
                             int width, int height,
                             int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y,
                             bool swap_xy) {
    auto hardware = DisplayHardwareDescriptor{
        .type = DisplayHardwareType::Rgb,
        .panel_io = panel_io,
        .panel = panel,
        .width = width,
        .height = height,
        .offset_x = offset_x,
        .offset_y = offset_y,
        .mirror_x = mirror_x,
        .mirror_y = mirror_y,
        .swap_xy = swap_xy,
    };
    return CreateDisplay(hardware);
}

Display* CreateMipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_handle_t panel,
                              int width, int height,
                              int offset_x, int offset_y,
                              bool mirror_x, bool mirror_y,
                              bool swap_xy) {
    auto hardware = DisplayHardwareDescriptor{
        .type = DisplayHardwareType::Mipi,
        .panel_io = panel_io,
        .panel = panel,
        .width = width,
        .height = height,
        .offset_x = offset_x,
        .offset_y = offset_y,
        .mirror_x = mirror_x,
        .mirror_y = mirror_y,
        .swap_xy = swap_xy,
    };
    return CreateDisplay(hardware);
}
