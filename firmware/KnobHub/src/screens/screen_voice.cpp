// Discord voice channel, styled like Discord's dark theme: a header with the
// server and channel, then members with their avatars. A green ring marks
// whoever is talking; slashed mic / headphone icons mark muted / deafened.
// Along the bottom, the whole cap below a line is one button: mute, leave or
// deafen, whichever the dial has picked.
#include "screens.h"
#include "../ui/gfx.h"
#include "../services/pc_link.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

const uint16_t DC_BG       = rgb(49, 51, 56);
const uint16_t DC_HEADER   = rgb(43, 45, 49);
const uint16_t DC_DIVIDER  = rgb(30, 31, 34);
const uint16_t DC_NAME     = rgb(181, 186, 193);
const uint16_t DC_BRIGHT   = rgb(242, 243, 245);
const uint16_t DC_MUTED    = rgb(148, 155, 164);
const uint16_t DC_SPEAKING = rgb(35, 165, 90);
const uint16_t DC_RED      = rgb(242, 63, 67);

// Tightened so four rows fit between the home band and the action band.
#define HEADER_BOTTOM 122
#define GUILD_Y       91
#define TITLE_Y       115
#define ROW_FIRST_Y   146   // centre of the first row
#define ROW_H         40    // fits an avatar plus its speaking ring without touching the next row
#define AVATAR_X      96
#define AVATAR_R      (AVATAR_SIZE / 2)
#define NAME_X        124
#define ICON_X        268
#define BAR_TEXT_Y    (BOTTOM_ICON_Y + 6)   // label baseline
#define BAR_DOTS_Y    (BOTTOM_ICON_Y + 20)

void draw_unplugged();

// Fallback avatar colours, like Discord's default avatars.
static const uint16_t AVATAR_COLORS[] = {
    rgb(88, 101, 242), rgb(116, 127, 141), rgb(59, 165, 93),
    rgb(250, 166, 26), rgb(237, 66, 69), rgb(235, 69, 158),
};

static void draw_background()
{
    canvas->fillScreen(DC_BG);
    canvas->fillRect(0, 0, LCD_W, HEADER_BOTTOM, DC_HEADER);
    canvas->fillRect(0, HEADER_BOTTOM, LCD_W, 2, DC_DIVIDER);
    draw_home_button();
}

// Speaker with two sound waves, its left edge at x.
static void draw_speaker(int x, int cy, uint16_t c)
{
    canvas->fillRect(x, cy - 3, 4, 7, c);
    canvas->fillTriangle(x + 2, cy, x + 9, cy - 7, x + 9, cy + 7, c);
    draw_arc(x + 10, cy, 3, 5, 45, 90, c);
    draw_arc(x + 10, cy, 7, 9, 45, 90, c);
}

// The header line: speaker icon and channel name, centred as one unit.
static void draw_channel_title(const char *name)
{
    char buf[48];
    strlcpy(buf, name, sizeof(buf));
    canvas->setFont(&FreeSansBold12pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    canvas->getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    w = min<int>(w, 200);
    int x = CENTER - (w + 26) / 2;
    draw_speaker(x, TITLE_Y - 7, DC_MUTED);
    draw_left(buf, &FreeSansBold12pt7b, x + 26, TITLE_Y, DC_BRIGHT, 200);
}

static void draw_avatar(const VoiceUser &u, int cy)
{
    if (u.speaking) {
        canvas->fillCircle(AVATAR_X, cy, AVATAR_R + 3, DC_SPEAKING);
        canvas->fillCircle(AVATAR_X, cy, AVATAR_R + 1, DC_BG);
    }
    const uint16_t *px = pc_avatar(u.id);
    if (!px) {
        uint32_t hash = 0;
        for (const char *p = u.id; *p; p++) hash = hash * 31 + *p;
        canvas->fillCircle(AVATAR_X, cy, AVATAR_R, AVATAR_COLORS[hash % 6]);
        char initial[2] = {(char)toupper(u.name[0] ? u.name[0] : '?'), 0};
        draw_centered(initial, &FreeSansBold9pt7b, cy + 6, COL_TEXT, 20, AVATAR_X);
        return;
    }
    // Copy the square avatar through a circular mask.
    int x0 = AVATAR_X - AVATAR_R, y0 = cy - AVATAR_R;
    for (int y = 0; y < AVATAR_SIZE; y++) {
        float dy = y + 0.5f - AVATAR_R;
        for (int x = 0; x < AVATAR_SIZE; x++) {
            float dx = x + 0.5f - AVATAR_R;
            if (dx * dx + dy * dy <= AVATAR_R * AVATAR_R) fb[(y0 + y) * LCD_W + x0 + x] = px[y * AVATAR_SIZE + x];
        }
    }
}

static void draw_slash(int cx, int cy)
{
    for (int t = 0; t < 2; t++) canvas->drawLine(cx - 7 + t, cy + 7, cx + 7 + t, cy - 7, DC_RED);
}

static void draw_mic(int cx, int cy, uint16_t c, bool off)
{
    canvas->fillRoundRect(cx - 3, cy - 8, 6, 10, 3, c);
    draw_arc(cx, cy, 5, 7, 90, 180, c);
    canvas->fillRect(cx - 1, cy + 6, 2, 3, c);
    if (off) draw_slash(cx, cy);
}

static void draw_headphones(int cx, int cy, uint16_t c, bool off)
{
    draw_arc(cx, cy + 1, 6, 8, 270, 180, c);
    canvas->fillRoundRect(cx - 8, cy, 4, 7, 1, c);
    canvas->fillRoundRect(cx + 4, cy, 4, 7, 1, c);
    if (off) draw_slash(cx, cy);
}

// Hung-up handset, like Discord's disconnect button.
static void draw_handset(int cx, int cy, uint16_t c)
{
    draw_arc(cx, cy + 9, 8, 12, 300, 120, c);   // bowed upward
    canvas->fillRoundRect(cx - 12, cy + 1, 6, 6, 2, c);
    canvas->fillRoundRect(cx + 6, cy + 1, 6, 6, 2, c);
}

static void draw_action_bar(VoiceAction action, const VoiceState &v, bool armed)
{
    // Deafened implies muted, so the mic shows off too.
    bool muted = v.self_mute || v.self_deaf;
    // Same see-through band as the home button; red-tinted when it's "on" or leaving.
    int red;   // how far toward red, 0..256
    const char *label;
    switch (action) {
    case VOICE_MUTE:   red = muted ? 70 : 0;       label = muted ? "Unmute" : "Mute"; break;
    case VOICE_DEAFEN: red = v.self_deaf ? 70 : 0; label = v.self_deaf ? "Undeafen" : "Deafen"; break;
    default:           red = armed ? 210 : 130;    label = armed ? "Tap again to leave" : "Leave call"; break;
    }
    if (red) draw_band(false, DC_RED, red);
    else draw_band(false);

    // Icon and label, centred as one unit.
    char buf[24];
    strlcpy(buf, label, sizeof(buf));
    canvas->setFont(&FreeSansBold9pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    canvas->getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    int icon_w = action == VOICE_LEAVE ? 24 : 16;
    int x = CENTER - (icon_w + 8 + w) / 2;
    int icon_cx = x + icon_w / 2, icon_cy = BAR_TEXT_Y - 6;
    if (action == VOICE_MUTE) draw_mic(icon_cx, icon_cy, DC_BRIGHT, muted);
    else if (action == VOICE_DEAFEN) draw_headphones(icon_cx, icon_cy, DC_BRIGHT, v.self_deaf);
    else draw_handset(icon_cx, icon_cy - 4, DC_BRIGHT);
    draw_left(buf, &FreeSansBold9pt7b, x + icon_w + 8, BAR_TEXT_Y, DC_BRIGHT, 200);

    // Which of the three is showing.
    for (int i = 0; i < VOICE_ACTION_COUNT; i++)
        canvas->fillCircle(CENTER + (i - 1) * 12, BAR_DOTS_Y, 2, i == action ? DC_BRIGHT : scale565(DC_BRIGHT, 110));
}


static void draw_status(const char *title, const char *line1, const char *line2)
{
    draw_centered(title, &FreeSansBold12pt7b, 112, DC_BRIGHT, 220);
    draw_centered(line1, &FreeSansBold12pt7b, 194, DC_BRIGHT, 260);
    draw_centered(line2, &FreeSans9pt7b, 222, DC_NAME, 260);
}

void voice_render(int first, VoiceAction action, bool leave_armed)
{
    draw_background();
    if (!pc_connected()) {
        draw_centered("Discord", &FreeSansBold12pt7b, 112, DC_BRIGHT, 220);
        draw_unplugged();
        return;
    }

    const VoiceState &v = pc().voice;
    switch (v.status) {
    case VOICE_STARTING:   draw_status("Discord", "Connecting", "Looking for Discord..."); return;
    case VOICE_NO_CONFIG:  draw_status("Discord", "Not set up", "Fill in discord.json"); return;
    case VOICE_NO_DISCORD: draw_status("Discord", "Discord isn't running", "Open Discord on your PC"); return;
    case VOICE_APPROVE:    draw_status("Discord", "Check Discord", "Approve the knob's access"); return;
    case VOICE_DENIED:     draw_status("Discord", "Access denied", "Restart the agent to retry"); return;
    case VOICE_ERROR:      draw_status("Discord", "Discord error", "See pc_agent.log"); return;
    case VOICE_OK:         break;
    }
    if (!v.in_channel) {
        draw_status("Discord", "Not in a voice channel", "Join one and it shows here");
        return;
    }

    draw_centered(v.guild, &FreeSans9pt7b, GUILD_Y, DC_MUTED, 180);
    draw_channel_title(v.channel);

    for (int row = 0; row < VOICE_ROWS && first + row < v.user_count; row++) {
        const VoiceUser &u = v.users[first + row];
        int cy = ROW_FIRST_Y + row * ROW_H;
        draw_avatar(u, cy);
        // Like Discord, deafened shows only the headphones (it implies muted).
        bool icon = u.deaf || u.muted;
        draw_left(u.name, &FreeSans9pt7b, NAME_X, cy + 5, u.speaking ? DC_BRIGHT : DC_NAME, icon ? 120 : 140);
        if (u.deaf) draw_headphones(ICON_X, cy, DC_MUTED, true);
        else if (u.muted) draw_mic(ICON_X, cy, DC_MUTED, true);
    }

    // More members below: a count beside the last row's icon column.
    int hidden = v.user_count - first - VOICE_ROWS;
    if (hidden > 0) {
        char s[8];
        snprintf(s, sizeof(s), "+%d", hidden);
        draw_left(s, &FreeSans9pt7b, ICON_X + 14, ROW_FIRST_Y + (VOICE_ROWS - 1) * ROW_H + 5, DC_MUTED, 30);
    }
    draw_action_bar(action, v, leave_armed);
}
