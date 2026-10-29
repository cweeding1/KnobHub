#include "gfx.h"

GFXcanvas16 *canvas;
uint16_t *fb;

struct RingPixel {
    uint32_t index;
    uint16_t angle;   // 0..65535, clockwise from 12 o'clock
};
static RingPixel *ring;
static int ring_count;

static float angle_deg(float dx, float dy)
{
    float a = atan2f(dx, -dy) * (180.0f / PI);
    return a < 0 ? a + 360.0f : a;
}

void gfx_begin()
{
    canvas = new GFXcanvas16(LCD_W, LCD_H);
    fb = canvas->getBuffer();
    canvas->setTextWrap(false);

    // The outer ring is redrawn often, so precompute its pixels and their angles.
    for (int pass = 0; pass < 2; pass++) {
        ring_count = 0;
        for (int y = 0; y < LCD_H; y++) {
            for (int x = 0; x < LCD_W; x++) {
                float dx = x + 0.5f - CENTER, dy = y + 0.5f - CENTER;
                float d2 = dx * dx + dy * dy;
                if (d2 < RING_IN * RING_IN || d2 >= RING_OUT * RING_OUT) continue;
                if (pass == 1)
                    ring[ring_count] = {(uint32_t)(y * LCD_W + x), (uint16_t)(angle_deg(dx, dy) / 360.0f * 65535)};
                ring_count++;
            }
        }
        if (pass == 0) ring = (RingPixel *)ps_malloc(ring_count * sizeof(RingPixel));
    }
}

void gfx_push()
{
    display_draw_bitmap(0, 0, LCD_W, LCD_H, fb);
}

uint16_t scale565(uint16_t c, int k)
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * k >> 8) << 11) | ((g * k >> 8) << 5) | (b * k >> 8);
}

uint16_t blend565(uint16_t c, uint16_t target, int k)
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r += (((target >> 11) & 31) - r) * k / 256;
    g += (((target >> 5) & 63) - g) * k / 256;
    b += ((target & 31) - b) * k / 256;
    return (r << 11) | (g << 5) | b;
}

static int chord_half_width(int y)
{
    float dy = y + 0.5f - CENTER;
    return (int)sqrtf(max(0.0f, CENTER * CENTER - dy * dy));
}

void draw_band(bool top, uint16_t tint, int k)
{
    int y0 = top ? 0 : BOTTOM_LINE_Y + BAND_LINE_W;
    int y1 = top ? HOME_LINE_Y : LCD_H;
    int line_y = top ? HOME_LINE_Y : BOTTOM_LINE_Y;
    for (int y = y0; y < y1; y++) {
        int hw = chord_half_width(y);
        for (int x = CENTER - hw; x < CENTER + hw; x++) fb[y * LCD_W + x] = blend565(fb[y * LCD_W + x], tint, k);
    }
    for (int y = line_y; y < line_y + BAND_LINE_W; y++) {
        int hw = chord_half_width(y);
        canvas->drawFastHLine(CENTER - hw, y, 2 * hw, rgb(100, 100, 100));
    }
}

bool hit_bottom_band(int x, int y)
{
    return y >= BOTTOM_LINE_Y;
}

void dim_disc(int r, int k, int cx, int cy)
{
    for (int y = cy - r; y < cy + r; y++) {
        float dy = y + 0.5f - cy;
        int hw = (int)sqrtf(max(0.0f, r * r - dy * dy));
        for (int x = cx - hw; x < cx + hw; x++) fb[y * LCD_W + x] = scale565(fb[y * LCD_W + x], k);
    }
}

static int text_width(const char *s, int16_t *x1 = nullptr)
{
    int16_t bx, by;
    uint16_t w, h;
    canvas->getTextBounds(s, 0, 0, &bx, &by, &w, &h);
    if (x1) *x1 = bx;
    return w;
}

// Copies s into buf, shortened with "..." until it fits max_w in the current font.
static int fit_text(const char *s, char *buf, size_t n, int max_w, int16_t *x1)
{
    strlcpy(buf, s, n);
    int w = text_width(buf, x1);
    for (int len = strlen(buf); w > max_w && len > 0; len--) {
        snprintf(buf, n, "%.*s...", len, s);
        w = text_width(buf, x1);
    }
    return w;
}

void draw_centered(const char *s, const GFXfont *font, int baseline, uint16_t color, int max_w, int cx)
{
    char buf[100];
    int16_t x1;
    canvas->setFont(font);
    int w = fit_text(s, buf, sizeof(buf), max_w, &x1);
    canvas->setTextColor(color);
    canvas->setCursor(cx - w / 2 - x1, baseline);
    canvas->print(buf);
}

void draw_left(const char *s, const GFXfont *font, int x, int baseline, uint16_t color, int max_w)
{
    char buf[100];
    int16_t x1;
    canvas->setFont(font);
    fit_text(s, buf, sizeof(buf), max_w, &x1);
    canvas->setTextColor(color);
    canvas->setCursor(x - x1, baseline);
    canvas->print(buf);
}

int draw_wrapped(const char *s, const GFXfont *font, int first_baseline, int line_h, uint16_t color, int max_w, int max_lines)
{
    canvas->setFont(font);
    char cur[100] = "";
    int lines = 0;
    const char *p = s;
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        const char *end = p;
        while (*end && *end != ' ') end++;
        char trial[100];
        snprintf(trial, sizeof(trial), "%s%s%.*s", cur, *cur ? " " : "", (int)(end - p), p);
        if (text_width(trial) <= max_w || !*cur) {
            strlcpy(cur, trial, sizeof(cur));
            p = end;
            continue;
        }
        // The word doesn't fit: flush the line. On the last line, mark the cut with "...".
        bool last = lines == max_lines - 1;
        if (last) {
            strlcat(cur, " ", sizeof(cur));
            strlcat(cur, p, sizeof(cur));   // draw_centered truncates it with "..."
        }
        draw_centered(cur, font, first_baseline + lines * line_h, color, max_w);
        lines++;
        cur[0] = 0;
        if (last) return lines;
    }
    if (*cur && lines < max_lines) {
        draw_centered(cur, font, first_baseline + lines * line_h, color, max_w);
        lines++;
    }
    return lines;
}

void draw_outer_ring(float fraction, uint16_t fg)
{
    uint32_t limit = (uint32_t)(constrain(fraction, 0.0f, 1.0f) * 65535);
    for (int i = 0; i < ring_count; i++) fb[ring[i].index] = ring[i].angle < limit ? fg : COL_RING_BG;
}

void draw_arc(int cx, int cy, int r_in, int r_out, float start_deg, float sweep_deg, uint16_t color)
{
    if (sweep_deg <= 0) return;
    for (int y = cy - r_out; y < cy + r_out; y++) {
        for (int x = cx - r_out; x < cx + r_out; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d2 = dx * dx + dy * dy;
            if (d2 < r_in * r_in || d2 >= r_out * r_out) continue;
            float rel = fmodf(angle_deg(dx, dy) - start_deg + 360.0f, 360.0f);
            if (rel < sweep_deg) fb[y * LCD_W + x] = color;
        }
    }
}

void draw_home_button()
{
    const int cx = CENTER, cy = HOME_ICON_Y;
    // A lighter cap across the top of the screen, closed off by a line under the icon.
    draw_band(true);
    canvas->fillTriangle(cx - 11, cy - 1, cx + 11, cy - 1, cx, cy - 11, COL_TEXT);   // roof
    canvas->fillRect(cx - 8, cy - 1, 16, 11, COL_TEXT);                             // walls
    canvas->fillRect(cx - 2, cy + 3, 4, 7, fb[(cy + 13) * LCD_W + cx]);              // door, in the backdrop colour
}

bool hit_home_button(int x, int y)
{
    return y < HOME_LINE_Y + BAND_LINE_W;
}
