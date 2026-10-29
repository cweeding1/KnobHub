#pragma once
// User settings, saved to flash. Each setting steps through a fixed list of options.
#include <Arduino.h>

enum Setting { SET_BRIGHTNESS, SET_DIM_AFTER, SET_DIM_TO, SET_OFF_AFTER, SET_LOCK_OFF, SET_NOTIFY_WAKE, SET_HAPTICS, SETTING_COUNT };

enum Haptics { HAPTICS_OFF, HAPTICS_LIGHT, HAPTICS_STRONG };

void settings_begin();
// Writes changes to flash a couple of seconds after the last one. Call often from loop().
void settings_save_if_needed();

// Percent, seconds (0 = never), 0/1 or a Haptics value, depending on the setting.
int setting(Setting s);
const char *setting_name(Setting s);
const char *setting_hint(Setting s);
// The current value as text, e.g. "40%", "2 min", "On".
void setting_label(Setting s, char *out, size_t n);
// How far through its options the setting is, 0..1, for the ring.
float setting_fraction(Setting s);
// On/off and named-choice settings, which a tap cycles through.
bool setting_is_toggle(Setting s);
// Moves through the options without wrapping. Returns false if already at the end.
bool setting_change(Setting s, int delta);
// Next option, wrapping around.
void setting_cycle(Setting s);
