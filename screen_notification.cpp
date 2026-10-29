// A Discord notification card drawn over whatever screen is showing.
#include "screens.h"
#include "gfx.h"
#include "pc_link.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

const uint16_t COL_CARD   = rgb(30, 31, 40);
const uint16_t COL_ACCENT = rgb(88, 101, 242);

void notification_draw()
{
    for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = scale565(fb[i], 70);
    canvas->fillCircle(CENTER, CENTER, 142, COL_ACCENT);
    canvas->fillCircle(CENTER, CENTER, 138, COL_CARD);

    const Notification &n = pc().notification;
    draw_centered("Discord", &FreeSans9pt7b, 92, rgb(140, 150, 255), 160);
    draw_centered(n.title, &FreeSansBold12pt7b, 128, COL_TEXT, 230);
    draw_wrapped(n.body, &FreeSans9pt7b, 162, 24, COL_SUBTEXT, 216, 5);
    draw_centered("Tap to dismiss", &FreeSans9pt7b, 292, rgb(100, 100, 110), 160);
}
