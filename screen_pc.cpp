#include "screens.h"
#include "gfx.h"
#include "pc_link.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

#define GAUGE_Y     160
#define GAUGE_R_IN  46
#define GAUGE_R_OUT 58
#define BAR_X       80
#define BAR_W       200

// Shared by the PC screens when the agent isn't sending anything.
void draw_unplugged()
{
    draw_centered("Plug into your PC", &FreeSansBold12pt7b, 176, COL_TEXT, 260);
    draw_centered("Shows while connected", &FreeSans9pt7b, 204, COL_SUBTEXT, 260);
}

// "45" followed by a small degree ring and "C", centred on cx.
static void draw_temperature(int cx, int baseline, float celsius, uint16_t color)
{
    char s[8];
    snprintf(s, sizeof(s), "%d", (int)roundf(celsius));
    canvas->setFont(&FreeSans9pt7b);
    int16_t x1, y1;
    uint16_t w, h, cw, ch;
    canvas->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    canvas->getTextBounds("C", 0, 0, &x1, &y1, &cw, &ch);
    int total = w + 7 + cw;
    int x = cx - total / 2;
    canvas->setTextColor(color);
    canvas->setCursor(x, baseline);
    canvas->print(s);
    canvas->drawCircle(x + w + 4, baseline - 11, 2, color);
    canvas->setCursor(x + w + 8, baseline);
    canvas->print("C");
}

// A 270-degree gauge with the gap at the bottom, where the label goes.
static void draw_gauge(int cx, const char *label, float pct, float temp, uint16_t color)
{
    draw_arc(cx, GAUGE_Y, GAUGE_R_IN, GAUGE_R_OUT, 225, 270, COL_RING_BG);
    draw_arc(cx, GAUGE_Y, GAUGE_R_IN, GAUGE_R_OUT, 225, 270 * constrain(pct, 0.0f, 100.0f) / 100, color);
    char s[8];
    snprintf(s, sizeof(s), "%d%%", (int)roundf(pct));
    bool has_temp = !isnan(temp);
    draw_centered(s, &FreeSansBold18pt7b, GAUGE_Y + (has_temp ? 6 : 12), COL_TEXT, 2 * GAUGE_R_IN - 6, cx);
    if (has_temp) draw_temperature(cx, GAUGE_Y + 30, temp, temp >= 85 ? rgb(255, 80, 80) : COL_SUBTEXT);
    draw_centered(label, &FreeSans9pt7b, GAUGE_Y + GAUGE_R_OUT + 2, COL_SUBTEXT, 60, cx);
}

static void draw_bar(int baseline, const char *label, float used, float total, uint16_t color)
{
    char s[40];
    snprintf(s, sizeof(s), "%s  %.1f / %.0f GB", label, used, total);
    draw_centered(s, &FreeSans9pt7b, baseline, COL_SUBTEXT, 260);
    int y = baseline + 6;
    canvas->fillRoundRect(BAR_X, y, BAR_W, 6, 3, COL_RING_BG);
    int w = total > 0 ? (int)(BAR_W * constrain(used / total, 0.0f, 1.0f)) : 0;
    if (w > 0) canvas->fillRoundRect(BAR_X, y, max(w, 6), 6, 3, color);
}

static void format_rate(float bps, char *out, size_t n)
{
    if (bps < 1024) snprintf(out, n, "%d B/s", (int)bps);
    else if (bps < 1024 * 1024) snprintf(out, n, "%.0f KB/s", bps / 1024);
    else snprintf(out, n, "%.1f MB/s", bps / (1024 * 1024));
}

void pc_render()
{
    canvas->fillScreen(COL_BG);
    draw_home_button();
    if (!pc_connected()) {
        draw_unplugged();
        return;
    }

    const PcStats &v = pc().stats;
    draw_centered(v.host, &FreeSans9pt7b, 96, rgb(120, 120, 120), 160);
    draw_gauge(CENTER - 68, "CPU", v.cpu, v.cpu_temp, COL_PC);
    draw_gauge(CENTER + 68, "GPU", v.gpu, v.gpu_temp, COL_GPU);
    draw_bar(250, "RAM", v.ram_used, v.ram_total, COL_PC);
    draw_bar(284, "VRAM", v.vram_used, v.vram_total, COL_GPU);

    char down[16], up[16], s[48];
    format_rate(v.net_down, down, sizeof(down));
    format_rate(v.net_up, up, sizeof(up));
    snprintf(s, sizeof(s), "Down %s   Up %s", down, up);
    draw_centered(s, &FreeSans9pt7b, 322, rgb(120, 120, 120), 220);
}
