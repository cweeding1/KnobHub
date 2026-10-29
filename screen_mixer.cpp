// Per-app volume. Dial adjusts it (the outer ring shows it while turning),
// the side buttons switch apps, tapping the middle mutes.
#include "screens.h"
#include "gfx.h"
#include "pc_link.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

void draw_unplugged();

const uint16_t COL_MIXER_RING = rgb(180, 110, 255);
const uint16_t COL_MUTED      = rgb(255, 80, 80);

// Also used by the settings screen.
void draw_chevron(int cx, bool right)
{
    dim_disc(SIDE_BUTTON_R, 110, cx, CENTER);
    int d = right ? 1 : -1;
    canvas->fillTriangle(cx - 5 * d, CENTER - 10, cx - 5 * d, CENTER + 10, cx + 7 * d, CENTER, COL_TEXT);
}

void mixer_render(int selected, bool gauge)
{
    canvas->fillScreen(COL_BG);
    const PcLink &p = pc();
    if (!pc_connected() || p.session_count == 0) {
        draw_home_button();
        draw_unplugged();
        return;
    }

    const MixerSession &s = p.sessions[selected];
    draw_home_button();

    draw_centered(s.name, &FreeSansBold12pt7b, 132, COL_TEXT, 200);
    char v[8];
    snprintf(v, sizeof(v), "%d", s.volume);
    draw_centered(v, &FreeSansBold24pt7b, 200, s.muted ? rgb(110, 110, 110) : COL_TEXT, 140);
    draw_centered(s.muted ? "Muted" : "Tap to mute", &FreeSans9pt7b, 232, s.muted ? COL_MUTED : rgb(110, 110, 110), 160);

    if (p.session_count > 1) {
        draw_chevron(SIDE_BUTTON_PREV_X, false);
        draw_chevron(SIDE_BUTTON_NEXT_X, true);
    }

    const int dot_y = 272, spacing = 14;
    int n = p.session_count;
    for (int i = 0; i < n; i++) {
        int x = CENTER + (2 * i - (n - 1)) * spacing / 2;
        canvas->fillCircle(x, dot_y, i == selected ? 4 : 3, i == selected ? COL_TEXT : rgb(80, 80, 80));
    }
    // Last, so it sits on top of everything.
    if (gauge) draw_outer_ring(s.volume / 100.0f, s.muted ? rgb(90, 90, 90) : COL_MIXER_RING);
}
