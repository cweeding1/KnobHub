#pragma once
// Shared drawing helpers. Every screen draws into one PSRAM canvas, then gfx_push() sends it to the panel.
#include <Adafruit_GFX.h>
#include "display.h"

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

const uint16_t COL_BG      = rgb(0, 0, 0);
const uint16_t COL_TEXT    = rgb(255, 255, 255);
const uint16_t COL_SUBTEXT = rgb(190, 190, 190);
const uint16_t COL_RING_BG = rgb(45, 45, 45);
const uint16_t COL_MUSIC   = rgb(30, 215, 96);
const uint16_t COL_PC      = rgb(60, 160, 255);
const uint16_t COL_GPU     = rgb(255, 140, 40);

#define CENTER   180
#define RING_IN  160
#define RING_OUT 174

// The home button is the whole cap of the screen above HOME_LINE_Y. Music and
// Discord mirror it with a button band of the same size along the bottom.
#define HOME_ICON_Y     46
#define HOME_LINE_Y     72
#define BAND_LINE_W     2
#define BOTTOM_LINE_Y   (LCD_H - HOME_LINE_Y - BAND_LINE_W)
#define BOTTOM_ICON_Y   (LCD_H - HOME_ICON_Y)   // centre of the bottom band's content

extern GFXcanvas16 *canvas;
extern uint16_t *fb;

void gfx_begin();
void gfx_push();

uint16_t scale565(uint16_t c, int k);     // k: 0..256
uint16_t blend565(uint16_t c, uint16_t target, int k);   // k: 0..256 of the way to target
// Tints the top or bottom band toward tint (by k, so what's behind shows
// through) and draws its dividing line.
void draw_band(bool top, uint16_t tint = COL_TEXT, int k = 36);
bool hit_bottom_band(int x, int y);
void dim_disc(int r, int k, int cx = CENTER, int cy = CENTER);
// Draws text centred on cx, shortened with "..." if it is wider than max_w.
void draw_centered(const char *s, const GFXfont *font, int baseline, uint16_t color, int max_w, int cx = CENTER);
// Draws text starting at x, shortened with "..." if it is wider than max_w.
void draw_left(const char *s, const GFXfont *font, int x, int baseline, uint16_t color, int max_w);
// Word-wraps text into up to max_lines centred lines. Returns the number of lines drawn.
int draw_wrapped(const char *s, const GFXfont *font, int first_baseline, int line_h, uint16_t color, int max_w, int max_lines);
// The ring around the edge of the screen, filled clockwise from 12 o'clock.
// Drawn over the screen's content, only briefly after the dial changes a value.
void draw_outer_ring(float fraction, uint16_t fg);
// An arc of a ring, angles in degrees clockwise from 12 o'clock.
void draw_arc(int cx, int cy, int r_in, int r_out, float start_deg, float sweep_deg, uint16_t color);
void draw_home_button();
bool hit_home_button(int x, int y);
