#include "screens.h"
#include "../ui/gfx.h"
#include "../services/spotify.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

const uint16_t COL_VOLUME = rgb(255, 255, 255);
const uint16_t COL_NO_ART = rgb(35, 35, 35);

const uint16_t COL_PROGRESS_BG = rgb(80, 80, 80);

// Title, artist and progress sit between the home band and the like band.
#define TITLE_Y    234
#define ARTIST_Y   256
#define PROGRESS_Y 270
#define PROGRESS_W 120

static int16_t art_half_width[LCD_H];
static int16_t art_source[LCD_W];   // which art pixel each screen pixel samples
static bool art_tables_ready;

// Stretches the art over the whole round screen, darkening the lower part so
// the text stays readable.
static void draw_art(const uint16_t *art, bool has_art)
{
    if (!art_tables_ready) {
        for (int i = 0; i < LCD_W; i++) {
            float d = i + 0.5f - CENTER;
            art_half_width[i] = (int16_t)sqrtf(max(0.0f, (float)(CENTER * CENTER) - d * d));
            art_source[i] = (int16_t)(i * ART_SIZE / LCD_W);
        }
        art_tables_ready = true;
    }
    for (int y = 0; y < LCD_H; y++) {
        int k = 256;
        if (y > 190) k = max(100, 256 - (y - 190) * 156 / 50);
        int hw = art_half_width[y];
        uint16_t *dst = fb + y * LCD_W;
        const uint16_t *src = art + art_source[y] * ART_SIZE;
        for (int x = CENTER - hw; x < CENTER + hw; x++) {
            uint16_t c = has_art ? src[art_source[x]] : COL_NO_ART;
            dst[x] = k == 256 ? c : scale565(c, k);
        }
    }
}

static void draw_progress(float fraction)
{
    int x = CENTER - PROGRESS_W / 2;
    int fill = (int)(constrain(fraction, 0.0f, 1.0f) * PROGRESS_W);
    canvas->fillRoundRect(x, PROGRESS_Y, PROGRESS_W, 4, 2, COL_PROGRESS_BG);
    if (fill > 0) canvas->fillRoundRect(x, PROGRESS_Y, max(fill, 4), 4, 2, COL_MUSIC);
}

// Two triangles and a bar (>>| or |<<) centred on cx, cy, h pixels tall each way.
static void draw_skip_glyph(int cx, int cy, int h, UiIcon dir)
{
    int w = h * 4 / 3, bar = max(2, h / 3);
    int x0 = cx - (2 * w + bar) / 2;
    if (dir == ICON_NEXT) {
        canvas->fillTriangle(x0, cy - h, x0, cy + h, x0 + w, cy, COL_TEXT);
        canvas->fillTriangle(x0 + w, cy - h, x0 + w, cy + h, x0 + 2 * w, cy, COL_TEXT);
        canvas->fillRect(x0 + 2 * w, cy - h, bar, 2 * h, COL_TEXT);
    } else {
        int x1 = x0 + bar;
        canvas->fillRect(x0, cy - h, bar, 2 * h, COL_TEXT);
        canvas->fillTriangle(x1 + w, cy - h, x1 + w, cy + h, x1, cy, COL_TEXT);
        canvas->fillTriangle(x1 + 2 * w, cy - h, x1 + 2 * w, cy + h, x1 + w, cy, COL_TEXT);
    }
}

static void draw_side_buttons()
{
    dim_disc(SIDE_BUTTON_R, 110, SIDE_BUTTON_PREV_X, CENTER);
    dim_disc(SIDE_BUTTON_R, 110, SIDE_BUTTON_NEXT_X, CENTER);
    draw_skip_glyph(SIDE_BUTTON_PREV_X, CENTER, 9, ICON_PREV);
    draw_skip_glyph(SIDE_BUTTON_NEXT_X, CENTER, 9, ICON_NEXT);
}

static void draw_play_icon()
{
    dim_disc(42, 90);
    canvas->fillTriangle(168, 160, 168, 200, 200, 180, COL_TEXT);
}

static void draw_skip_icon(UiIcon icon)
{
    dim_disc(48, 90);
    draw_skip_glyph(CENTER, CENTER, 18, icon);
}

static void draw_volume(int volume, bool supported)
{
    dim_disc(64, 80);
    char s[8];
    if (supported && volume >= 0) snprintf(s, sizeof(s), "%d", volume);
    else strcpy(s, "--");
    draw_centered(s, &FreeSansBold24pt7b, 196, COL_TEXT, 120);
    if (!supported) draw_centered("No volume control", &FreeSans9pt7b, 222, COL_SUBTEXT, 200);
}

static bool in_heart(float dx, float dy, float r)
{
    // Two round lobes on top of a downward triangle.
    float lx = fabsf(dx) - r * 0.48f, ly = dy + r * 0.25f, cr = r * 0.52f;
    if (lx * lx + ly * ly <= cr * cr) return true;
    float top = -r * 0.05f, tip = r * 0.9f;
    return dy >= top && dy <= tip && fabsf(dx) <= r * 0.98f * (tip - dy) / (tip - top);
}

// A filled heart, or just its outline.
static void draw_heart(int cx, int cy, int r, uint16_t c, bool filled)
{
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (in_heart(dx, dy, r) && (filled || !in_heart(dx, dy, r - 2.5f))) fb[y * LCD_W + x] = c;
        }
    }
}

// Mirrors the home band at the top: the whole bottom cap is one button.
static void draw_like_bar(bool saved)
{
    draw_band(false);
    draw_heart(CENTER, BOTTOM_ICON_Y, 12, saved ? COL_MUSIC : COL_TEXT, saved);
}

static void draw_message(const char *line1, const char *line2)
{
    canvas->fillScreen(COL_BG);
    draw_home_button();
    draw_centered(line1, &FreeSansBold12pt7b, 176, COL_TEXT, 260);
    draw_centered(line2, &FreeSans9pt7b, 204, COL_SUBTEXT, 260);
}

void music_render(const UiOverlay &overlay)
{
    spotify_lock();
    PlayerState s = spotify_state();
    if (s.status == SP_OK) {
        canvas->fillScreen(COL_BG);
        draw_art(spotify_art(), s.has_art);
    }
    spotify_unlock();

    switch (s.status) {
    case SP_NEED_SETUP:      draw_message("Setup needed", "See README: Spotify"); break;
    case SP_WIFI_CONNECTING: draw_message("Connecting", "Joining Wi-Fi..."); break;
    case SP_AUTH_FAILED:     draw_message("Spotify login failed", "Run spotify_auth.py again"); break;
    case SP_NO_PLAYER:       draw_message("Nothing playing", "Start Spotify on any device"); break;
    case SP_OK: {
        uint32_t progress = s.progress_ms + (s.playing ? millis() - s.progress_at : 0);
        draw_progress(s.duration_ms ? (float)progress / s.duration_ms : 0);

        draw_centered(s.title, &FreeSansBold12pt7b, TITLE_Y, COL_TEXT, 240);
        draw_centered(s.artist, &FreeSans9pt7b, ARTIST_Y, COL_SUBTEXT, 230);
        draw_side_buttons();
        draw_home_button();
        if (s.can_save) draw_like_bar(s.saved);

        if (overlay.volume)                draw_volume(s.volume, s.supports_volume);
        else if (overlay.icon != ICON_NONE) draw_skip_icon(overlay.icon);
        else if (!s.playing)               draw_play_icon();
        // Last, so it sits on top of everything.
        if (overlay.volume) draw_outer_ring(s.volume < 0 ? 0 : s.volume / 100.0f, COL_VOLUME);
        break;
    }
    }
}
