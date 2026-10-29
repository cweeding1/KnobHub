// Settings, one per page like the mixer: the dial changes the value, the side
// buttons switch settings, tapping the middle flips on/off settings.
#include "screens.h"
#include "../ui/gfx.h"
#include "../services/settings.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

void draw_chevron(int cx, bool right);

const uint16_t COL_SETTINGS = rgb(255, 176, 50);
const uint16_t COL_DIMMED   = rgb(110, 110, 110);

void settings_render(int page, bool gauge)
{
    canvas->fillScreen(COL_BG);
    Setting s = (Setting)page;
    bool off = setting_is_toggle(s) && setting(s) == 0;
    draw_home_button();

    draw_centered(setting_name(s), &FreeSansBold12pt7b, 132, COL_TEXT, 240);
    char v[16];
    setting_label(s, v, sizeof(v));
    draw_centered(v, &FreeSansBold24pt7b, 200, off ? COL_DIMMED : COL_TEXT, 170);
    draw_centered(setting_hint(s), &FreeSans9pt7b, 232, COL_DIMMED, 200);

    draw_chevron(SIDE_BUTTON_PREV_X, false);
    draw_chevron(SIDE_BUTTON_NEXT_X, true);

    const int dot_y = 272, spacing = 14;
    for (int i = 0; i < SETTING_COUNT; i++) {
        int x = CENTER + (2 * i - (SETTING_COUNT - 1)) * spacing / 2;
        canvas->fillCircle(x, dot_y, i == page ? 4 : 3, i == page ? COL_TEXT : rgb(80, 80, 80));
    }
    // Last, so it sits on top of everything.
    if (gauge) draw_outer_ring(setting_fraction(s), COL_SETTINGS);
}
