#ifndef UI_DISPLAY_FACTORY_H
#define UI_DISPLAY_FACTORY_H

#include "boards/common/display_hardware.h"

class Display;

Display* CreateDisplay(const DisplayHardwareDescriptor& hardware);

// Compatibility wrappers for board variants that have not yet migrated to
// DisplayHardwareDescriptor. New code should call CreateDisplay directly.
Display* CreateNoDisplay();
Display* CreateOledDisplay(esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_handle_t panel,
                           int width, int height,
                           bool mirror_x, bool mirror_y);
Display* CreateSpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_panel_handle_t panel,
                             int width, int height,
                             int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y,
                             bool swap_xy);
Display* CreateRgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_panel_handle_t panel,
                             int width, int height,
                             int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y,
                             bool swap_xy);
Display* CreateMipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_handle_t panel,
                              int width, int height,
                              int offset_x, int offset_y,
                              bool mirror_x, bool mirror_y,
                              bool swap_xy);

#endif  // UI_DISPLAY_FACTORY_H
