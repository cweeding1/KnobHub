// Home carousel: the selected app as a big tile in the middle, its neighbours
// peeking in from the sides, and page dots along the bottom.
#include "screens.h"
#include "gfx.h"
#include "pc_link.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

const App HOME_APPS[] = {APP_MUSIC, APP_PC, APP_MIXER, APP_VOICE, APP_SETTINGS};
const int HOME_APP_COUNT = sizeof(HOME_APPS) / sizeof(HOME_APPS[0]);

const uint16_t COL_MIXER = rgb(180, 110, 255);
const uint16_t COL_VOICE = rgb(88, 101, 242);
const uint16_t COL_TILE  = rgb(28, 28, 28);
const uint16_t COL_GEAR  = rgb(255, 176, 50);

#define TILE_Y      165
#define BIG_R       66
#define SMALL_R     30
#define SMALL_X_OFF 128

typedef void (*IconFn)(int cx, int cy, float s, uint16_t c, uint16_t bg);

static int sc(float v, float s) { return (int)roundf(v * s); }

static void icon_music(int cx, int cy, float s, uint16_t c, uint16_t)
{
    canvas->fillCircle(cx + sc(-12, s), cy + sc(14, s), sc(9, s), c);
    canvas->fillCircle(cx + sc(14, s), cy + sc(8, s), sc(9, s), c);
    canvas->fillRect(cx + sc(-7, s), cy + sc(-18, s), max(2, sc(4, s)), sc(32, s), c);
    canvas->fillRect(cx + sc(19, s), cy + sc(-24, s), max(2, sc(4, s)), sc(32, s), c);
    canvas->fillTriangle(cx + sc(-7, s), cy + sc(-18, s), cx + sc(23, s), cy + sc(-24, s), cx + sc(23, s), cy + sc(-17, s), c);
    canvas->fillTriangle(cx + sc(-7, s), cy + sc(-18, s), cx + sc(23, s), cy + sc(-17, s), cx + sc(-7, s), cy + sc(-11, s), c);
}

static void icon_pc(int cx, int cy, float s, uint16_t c, uint16_t)
{
    canvas->fillRect(cx + sc(-22, s), cy + sc(6, s), sc(10, s), sc(14, s), c);
    canvas->fillRect(cx + sc(-5, s), cy + sc(-6, s), sc(10, s), sc(26, s), c);
    canvas->fillRect(cx + sc(12, s), cy + sc(-18, s), sc(10, s), sc(38, s), c);
    canvas->fillRect(cx + sc(-26, s), cy + sc(22, s), sc(52, s), max(2, sc(3, s)), c);
}

static void icon_mixer(int cx, int cy, float s, uint16_t c, uint16_t)
{
    const int knob_y[3] = {8, -10, 2};
    for (int i = 0; i < 3; i++) {
        int x = cx + sc(-16 + 16 * i, s);
        canvas->fillRect(x - max(1, sc(1.5f, s)), cy + sc(-22, s), max(2, sc(3, s)), sc(44, s), c);
        canvas->fillRoundRect(x - sc(7, s), cy + sc(knob_y[i] - 4, s), sc(14, s), sc(8, s), max(1, sc(2, s)), c);
    }
}

static void icon_voice(int cx, int cy, float s, uint16_t c, uint16_t bg)
{
    // Headset: a band over the top and two ear cups.
    canvas->fillCircle(cx, cy + sc(2, s), sc(22, s), c);
    canvas->fillCircle(cx, cy + sc(2, s), sc(17, s), bg);
    canvas->fillRect(cx - sc(23, s), cy + sc(2, s), sc(46, s), sc(24, s), bg);
    canvas->fillRoundRect(cx - sc(25, s), cy - sc(2, s), sc(11, s), sc(20, s), max(1, sc(3, s)), c);
    canvas->fillRoundRect(cx + sc(14, s), cy - sc(2, s), sc(11, s), sc(20, s), max(1, sc(3, s)), c);
}

static void icon_settings(int cx, int cy, float s, uint16_t c, uint16_t bg)
{
    // Gear: eight teeth around a ring.
    for (int i = 0; i < 8; i++) draw_arc(cx, cy, sc(16, s), sc(26, s), i * 45 - 11, 22, c);
    canvas->fillCircle(cx, cy, sc(19, s), c);
    canvas->fillCircle(cx, cy, sc(8, s), bg);
}

struct AppInfo {
    const char *name;
    uint16_t accent;
    IconFn icon;
    bool needs_pc;
};

static AppInfo info(App app)
{
    switch (app) {
    case APP_MUSIC: return {"Music", COL_MUSIC, icon_music, false};
    case APP_PC:    return {"PC Stats", COL_PC, icon_pc, true};
    case APP_MIXER: return {"Mixer", COL_MIXER, icon_mixer, true};
    case APP_VOICE: return {"Discord", COL_VOICE, icon_voice, true};
    case APP_SETTINGS: return {"Settings", COL_GEAR, icon_settings, false};
    default:        return {"", COL_TEXT, icon_music, false};
    }
}

static void draw_small_tile(int cx, App app)
{
    canvas->fillCircle(cx, TILE_Y, SMALL_R, COL_TILE);
    info(app).icon(cx, TILE_Y, 0.55f, rgb(110, 110, 110), COL_TILE);
}

void home_render(int selected)
{
    canvas->fillScreen(COL_BG);
    int n = HOME_APP_COUNT;
    draw_small_tile(CENTER - SMALL_X_OFF, HOME_APPS[(selected + n - 1) % n]);
    draw_small_tile(CENTER + SMALL_X_OFF, HOME_APPS[(selected + 1) % n]);

    AppInfo app = info(HOME_APPS[selected]);
    uint16_t fill = scale565(app.accent, 70);
    canvas->fillCircle(CENTER, TILE_Y, BIG_R + 5, app.accent);
    canvas->fillCircle(CENTER, TILE_Y, BIG_R, fill);
    app.icon(CENTER, TILE_Y, 1.3f, COL_TEXT, fill);

    draw_centered(app.name, &FreeSansBold12pt7b, TILE_Y + BIG_R + 36, COL_TEXT, 220);
    if (app.needs_pc) {
        bool live = pc_connected();
        draw_centered(live ? "Connected" : "Plug into PC", &FreeSans9pt7b, TILE_Y + BIG_R + 60,
                      live ? COL_MUSIC : rgb(120, 120, 120), 200);
    }

    const int dot_y = 322, spacing = 16;
    for (int i = 0; i < n; i++) {
        int x = CENTER + (2 * i - (n - 1)) * spacing / 2;
        canvas->fillCircle(x, dot_y, i == selected ? 4 : 3, i == selected ? COL_TEXT : rgb(80, 80, 80));
    }
}

int home_hit(int x, int y)
{
    auto inside = [&](int cx, int r) {
        int dx = x - cx, dy = y - TILE_Y;
        return dx * dx + dy * dy <= r * r;
    };
    if (inside(CENTER, BIG_R + 12)) return 0;
    if (inside(CENTER - SMALL_X_OFF, SMALL_R + 18)) return -1;
    if (inside(CENTER + SMALL_X_OFF, SMALL_R + 18)) return 1;
    return HOME_HIT_NONE;
}
