#include "settings.h"
#include <Preferences.h>

#define SAVE_DELAY_MS 2000

enum Kind { KIND_PERCENT, KIND_DURATION, KIND_ONOFF, KIND_CHOICE };

struct Def {
    const char *key;   // Preferences key, at most 15 characters
    const char *name, *hint;
    Kind kind;
    const int16_t *values;
    uint8_t count;
    int16_t def;
    const char *const *labels;   // KIND_CHOICE only
};

static const int16_t BRIGHTNESS[] = {5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100};
static const int16_t DIM_AFTER[] = {0, 15, 30, 60, 120, 300, 600};
static const int16_t DIM_TO[] = {5, 10, 20, 30, 50};
static const int16_t OFF_AFTER[] = {0, 60, 120, 300, 600, 1800, 3600};
static const int16_t ONOFF[] = {0, 1};
static const int16_t HAPTIC_LEVELS[] = {HAPTICS_OFF, HAPTICS_LIGHT, HAPTICS_STRONG};
static const char *const HAPTIC_NAMES[] = {"Off", "Light", "Strong"};

#define COUNT(a) (uint8_t)(sizeof(a) / sizeof(a[0]))

static const Def DEFS[SETTING_COUNT] = {
    {"bright",      "Brightness",         "Screen brightness",        KIND_PERCENT,  BRIGHTNESS,    COUNT(BRIGHTNESS),    100},
    {"dim_after",   "Dim after",          "Idle time before dimming", KIND_DURATION, DIM_AFTER,     COUNT(DIM_AFTER),     60},
    {"dim_to",      "Dim to",             "Brightness when dimmed",   KIND_PERCENT,  DIM_TO,        COUNT(DIM_TO),        10},
    {"off_after",   "Screen off after",   "Idle time before off",     KIND_DURATION, OFF_AFTER,     COUNT(OFF_AFTER),     600},
    {"lock_off",    "Off when PC locks",  "Needs the PC agent",       KIND_ONOFF,    ONOFF,         COUNT(ONOFF),         1},
    {"notify_wake", "Notifications wake", "Discord pop-ups light up", KIND_ONOFF,    ONOFF,         COUNT(ONOFF),         1},
    {"haptics",     "Haptics",            "Vibration strength",       KIND_CHOICE,   HAPTIC_LEVELS, COUNT(HAPTIC_LEVELS), HAPTICS_STRONG, HAPTIC_NAMES},
};

static Preferences prefs;
static uint8_t index_of[SETTING_COUNT];
static bool unsaved;
static uint32_t changed_at;

static int find_index(const Def &d, int value)
{
    for (int i = 0; i < d.count; i++)
        if (d.values[i] == value) return i;
    return -1;
}

void settings_begin()
{
    prefs.begin("settings");
    for (int s = 0; s < SETTING_COUNT; s++) {
        const Def &d = DEFS[s];
        // Values are stored rather than indexes, so editing an option list keeps saved settings.
        int i = find_index(d, prefs.getShort(d.key, d.def));
        index_of[s] = i >= 0 ? i : find_index(d, d.def);
    }
}

void settings_save_if_needed()
{
    if (!unsaved || millis() - changed_at < SAVE_DELAY_MS) return;
    for (int s = 0; s < SETTING_COUNT; s++)
        prefs.putShort(DEFS[s].key, setting((Setting)s));
    unsaved = false;
}

int setting(Setting s) { return DEFS[s].values[index_of[s]]; }
const char *setting_name(Setting s) { return DEFS[s].name; }
const char *setting_hint(Setting s) { return DEFS[s].hint; }

void setting_label(Setting s, char *out, size_t n)
{
    int v = setting(s);
    switch (DEFS[s].kind) {
    case KIND_PERCENT: snprintf(out, n, "%d%%", v); break;
    case KIND_ONOFF:   strlcpy(out, v ? "On" : "Off", n); break;
    case KIND_CHOICE:  strlcpy(out, DEFS[s].labels[index_of[s]], n); break;
    case KIND_DURATION:
        if (v == 0) strlcpy(out, "Never", n);
        else if (v < 60) snprintf(out, n, "%d sec", v);
        else if (v < 3600) snprintf(out, n, "%d min", v / 60);
        else snprintf(out, n, "%d hr", v / 3600);
        break;
    }
}

float setting_fraction(Setting s)
{
    return DEFS[s].count > 1 ? index_of[s] / (float)(DEFS[s].count - 1) : 0;
}

bool setting_is_toggle(Setting s)
{
    return DEFS[s].kind == KIND_ONOFF || DEFS[s].kind == KIND_CHOICE;
}

static void set_index(Setting s, int i)
{
    index_of[s] = i;
    unsaved = true;
    changed_at = millis();
}

bool setting_change(Setting s, int delta)
{
    int i = constrain(index_of[s] + delta, 0, DEFS[s].count - 1);
    if (i == index_of[s]) return false;
    set_index(s, i);
    return true;
}

void setting_cycle(Setting s)
{
    set_index(s, (index_of[s] + 1) % DEFS[s].count);
}
