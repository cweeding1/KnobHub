#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_W 360
#define LCD_H 360

// The panel only accepts windows that start on even coordinates and have
// even widths/heights, so keep every x, y, w, h passed here even.
void display_init(void);
void display_fill_rect(int x, int y, int w, int h, uint16_t rgb565);
void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *rgb565);
// 0 = off, 1 = full brightness.
void display_set_backlight(float level);

#ifdef __cplusplus
}
#endif
