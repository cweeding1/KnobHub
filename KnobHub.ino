// Multi-app dashboard for the Waveshare ESP32-S3 Knob Touch LCD 1.8.
//
// Home:      turn the dial to pick an app, tap the middle to open it. In every
//            app, the band across the top of the screen is the home button.
// Music:     Spotify now playing. Dial = volume, tap middle = play/pause,
//            tap right / swipe left = next, tap left / swipe right = restart
//            song (or previous track near the start). Tap the band along the
//            bottom to add / remove the song from Liked Songs.
// PC Stats:  CPU/GPU load and temperature, RAM, VRAM and network.
// Mixer:     dial = volume of the selected app, tap sides / swipe = switch
//            app, tap middle = mute.
// Discord:   your current voice channel, who's in it and who's talking.
//            Dial picks mute / leave / deafen in the bottom bar, tap the bar to
//            do it. Swipe up / down scrolls long member lists.
// Settings:  brightness, auto-dim, haptics. Dial = change value, tap sides /
//            swipe = next setting, tap middle = flip on/off settings.
// Discord notifications pop up over any screen; tap to dismiss.
// The screen dims and turns off when idle; the first touch or turn only wakes it.
//
// PC Stats, Mixer and Discord come from pc_agent.py over USB serial.
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include "SensorDRV2605.hpp"
#include "display.h"
#include "bidi_switch_knob.h"
#include "gfx.h"
#include "spotify.h"
#include "pc_link.h"
#include "screens.h"
#include "settings.h"

#define PIN_ENCODER_A 8
#define PIN_ENCODER_B 7
#define PIN_I2C_SDA   11
#define PIN_I2C_SCL   12
#define TOUCH_ADDR    0x15
// Set to 1 if turning the dial right lowers the volume.
#define SWAP_KNOB_DIRECTION 0

#define MUSIC_VOLUME_STEP 5
#define MIXER_VOLUME_STEP 2
#define SWIPE_MIN_PX      50
#define ROW_SCROLL_PX     40   // vertical swipe distance per Discord member row
#define TAP_MAX_PX        20
#define TAP_MAX_MS        400
#define VOLUME_SHOW_MS    1500
#define ICON_SHOW_MS      700
#define NOTIFICATION_MS   6000
#define LEAVE_CONFIRM_MS  3000
#define DIM_PREVIEW_MS    1500
#define FADE_UP_MS        150  // time for a full 0-100% brightness change
#define FADE_DOWN_MS      800
#define HAPTIC_CLICK        1  // DRV2605 library effects
#define HAPTIC_DOUBLE       10
#define HAPTIC_CLICK_LIGHT  3
#define HAPTIC_DOUBLE_LIGHT 11

// ---------- Dial ----------

static portMUX_TYPE knob_mux = portMUX_INITIALIZER_UNLOCKED;
static int knob_delta;

static void add_knob_delta(int d)
{
    if (SWAP_KNOB_DIRECTION) d = -d;
    portENTER_CRITICAL(&knob_mux);
    knob_delta += d;
    portEXIT_CRITICAL(&knob_mux);
}
static void on_knob_left(void *, void *)  { add_knob_delta(-1); }
static void on_knob_right(void *, void *) { add_knob_delta(+1); }

// ---------- Haptics ----------

static SensorDRV2605 haptic;
static bool has_haptic;

static void buzz(uint8_t effect = HAPTIC_CLICK)
{
    int strength = setting(SET_HAPTICS);
    if (!has_haptic || strength == HAPTICS_OFF) return;
    if (strength == HAPTICS_LIGHT) effect = effect == HAPTIC_DOUBLE ? HAPTIC_DOUBLE_LIGHT : HAPTIC_CLICK_LIGHT;
    haptic.setWaveform(0, effect);
    haptic.setWaveform(1, 0);
    haptic.run();
}

// ---------- Touch ----------

// Reads the first touch point from the CST816. Returns false when nothing is touching.
static bool read_touch(int &x, int &y)
{
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(TOUCH_ADDR, 5) != 5) return false;
    uint8_t d[5];
    for (int i = 0; i < 5; i++) d[i] = Wire.read();
    if ((d[0] & 0x0F) == 0) return false;
    x = ((d[1] & 0x0F) << 8) | d[2];
    y = ((d[3] & 0x0F) << 8) | d[4];
    return true;
}

// ---------- App state ----------

static Preferences prefs;
static App app = APP_HOME;
static int home_selected;
static char mixer_id[40] = "master";   // selected mixer session, by id so it survives reordering
static int voice_first;                // first visible Discord member
static VoiceAction voice_action;       // shown in the Discord action bar
static int settings_page;              // a Setting
static bool dirty = true;              // force a redraw on the next loop

static uint32_t volume_until, icon_until, notification_until, leave_armed_until;
static UiIcon icon;

static bool notifying()   { return (int32_t)(millis() - notification_until) < 0; }
static bool leave_armed() { return (int32_t)(millis() - leave_armed_until) < 0; }

// ---------- Backlight ----------

static uint32_t last_activity;       // last dial turn or touch
static uint32_t dim_preview_until;   // shows the "Dim to" level while it's being adjusted
static uint32_t locked_at;
static bool pc_locked;

// Brightness in percent from the idle timers, lock screen and notifications.
static int idle_brightness()
{
    int full = setting(SET_BRIGHTNESS);
    if (setting(SET_NOTIFY_WAKE) && notifying()) return full;
    // Locking the PC turns the screen off until the knob is touched.
    if (setting(SET_LOCK_OFF) && pc_locked && (int32_t)(last_activity - locked_at) < 0) return 0;
    uint32_t idle_s = (millis() - last_activity) / 1000;
    int off_after = setting(SET_OFF_AFTER), dim_after = setting(SET_DIM_AFTER);
    if (off_after && idle_s >= off_after) return 0;
    if (dim_after && idle_s >= dim_after) return min(setting(SET_DIM_TO), full);
    return full;
}

// Called on any input. Returns true if the screen was dimmed or off, in which
// case the input only wakes it and shouldn't do anything else.
static bool wake()
{
    bool asleep = idle_brightness() < setting(SET_BRIGHTNESS);
    last_activity = millis();
    return asleep;
}

static void track_pc_lock()
{
    bool locked = pc_connected() && pc().stats.locked;
    if (locked == pc_locked) return;
    pc_locked = locked;
    if (locked) locked_at = millis();
    else if (pc_connected()) last_activity = millis();   // unlocked: you're back at the PC
}

// Fades the backlight towards where it should be.
static void update_backlight()
{
    static float level = -1;   // percent
    static uint32_t last;
    uint32_t now = millis();
    bool previewing = (int32_t)(now - dim_preview_until) < 0;
    float target = previewing ? min(setting(SET_DIM_TO), setting(SET_BRIGHTNESS)) : idle_brightness();
    if (level < 0) level = target;
    float step = (now - last) * 100.0f / (target > level ? FADE_UP_MS : FADE_DOWN_MS);
    last = now;
    level = target > level ? min(target, level + step) : max(target, level - step);
    display_set_backlight(level / 100);
}

static void open_app(App a)
{
    app = a;
    prefs.putUChar("app", a);
    for (int i = 0; i < HOME_APP_COUNT; i++)
        if (HOME_APPS[i] == a) home_selected = i;
    voice_first = 0;
    dirty = true;
}

static bool player_ready()
{
    spotify_lock();
    bool ok = spotify_state().status == SP_OK;
    spotify_unlock();
    return ok;
}

// Index of the selected mixer session, falling back to the first one.
static int mixer_index()
{
    const PcLink &p = pc();
    for (int i = 0; i < p.session_count; i++)
        if (strcmp(p.sessions[i].id, mixer_id) == 0) return i;
    if (p.session_count) strlcpy(mixer_id, p.sessions[0].id, sizeof(mixer_id));
    return 0;
}

static void mixer_select(int step)
{
    const PcLink &p = pc();
    if (p.session_count == 0) return;
    int i = (mixer_index() + step + p.session_count) % p.session_count;
    strlcpy(mixer_id, p.sessions[i].id, sizeof(mixer_id));
    dirty = true;
    buzz();
}

static void skip(UiIcon dir)
{
    if (dir == ICON_NEXT) spotify_next();
    else spotify_previous();
    icon = dir;
    icon_until = millis() + ICON_SHOW_MS;
    buzz();
}

static void handle_dial(int delta)
{
    const PcLink &p = pc();
    switch (app) {
    case APP_HOME:
        home_selected = ((home_selected + delta) % HOME_APP_COUNT + HOME_APP_COUNT) % HOME_APP_COUNT;
        dirty = true;
        break;
    case APP_MUSIC:
        if (player_ready()) {
            spotify_change_volume(delta * MUSIC_VOLUME_STEP);
            volume_until = millis() + VOLUME_SHOW_MS;
        }
        break;
    case APP_MIXER:
        if (pc_connected() && p.session_count) {
            const MixerSession &s = p.sessions[mixer_index()];
            pc_set_volume(s.id, s.volume + delta * MIXER_VOLUME_STEP);
            volume_until = millis() + VOLUME_SHOW_MS;
        }
        break;
    case APP_VOICE:
        voice_action = (VoiceAction)(((voice_action + delta) % VOICE_ACTION_COUNT + VOICE_ACTION_COUNT) % VOICE_ACTION_COUNT);
        leave_armed_until = millis();
        dirty = true;
        break;
    case APP_SETTINGS: {
        Setting s = (Setting)settings_page;
        volume_until = millis() + VOLUME_SHOW_MS;   // the value's ring, even at a limit
        if (!setting_change(s, delta)) break;
        if (s == SET_DIM_TO) dim_preview_until = millis() + DIM_PREVIEW_MS;
        if (s == SET_HAPTICS) buzz();   // feel the new strength
        dirty = true;
        break;
    }
    case APP_PC:
        break;
    }
}

// x, y is where the finger went down; dx, dy is how far it moved.
static void handle_gesture(int x, int y, int dx, int dy, bool tap, bool swipe)
{
    if (notifying()) {
        if (tap) {
            notification_until = millis();
            dirty = true;
        }
        return;
    }
    if (tap && app != APP_HOME && hit_home_button(x, y)) {
        open_app(APP_HOME);
        buzz();
        return;
    }

    bool go_next = (swipe && dx < 0) || (tap && x >= SIDE_ZONE_NEXT_MIN);
    bool go_prev = (swipe && dx > 0) || (tap && x <= SIDE_ZONE_PREV_MAX);

    switch (app) {
    case APP_HOME:
        if (tap) {
            int hit = home_hit(x, y);
            if (hit == 0) {
                open_app(HOME_APPS[home_selected]);
                buzz();
            } else if (hit != HOME_HIT_NONE) {
                home_selected = (home_selected + hit + HOME_APP_COUNT) % HOME_APP_COUNT;
                dirty = true;
            }
        }
        break;
    case APP_MUSIC: {
        if (!player_ready()) break;
        spotify_lock();
        bool can_save = spotify_state().can_save;
        spotify_unlock();
        // Without the band (e.g. a local file), the bottom acts like the middle.
        if (tap && can_save && hit_bottom_band(x, y)) {
            spotify_toggle_saved();
            buzz();
            break;
        }
        if (go_next) skip(ICON_NEXT);
        else if (go_prev) skip(ICON_PREV);
        else if (tap) {
            spotify_toggle_play();
            buzz();
        }
        break;
    }
    case APP_MIXER:
        if (!pc_connected()) break;
        if (go_next) mixer_select(1);
        else if (go_prev) mixer_select(-1);
        else if (tap && pc().session_count) {
            const MixerSession &s = pc().sessions[mixer_index()];
            pc_set_mute(s.id, !s.muted);
            buzz();
        }
        break;
    case APP_VOICE: {
        const VoiceState &v = pc().voice;
        if (!pc_connected() || v.status != VOICE_OK || !v.in_channel) break;
        if (abs(dy) >= SWIPE_MIN_PX && abs(dy) > abs(dx)) {
            // Drag the list like a phone: swiping up shows members further down.
            int rows = max(1, abs(dy) / ROW_SCROLL_PX);
            int next = constrain(voice_first + (dy < 0 ? rows : -rows), 0, max(0, v.user_count - VOICE_ROWS));
            if (next != voice_first) {
                voice_first = next;
                dirty = true;
            }
            break;
        }
        if (!tap || !hit_bottom_band(x, y)) break;
        if (voice_action != VOICE_LEAVE) {
            if (voice_action == VOICE_MUTE) pc_set_voice_mute(!(v.self_mute || v.self_deaf));
            else pc_set_voice_deaf(!v.self_deaf);
            buzz();
            dirty = true;
            break;
        }
        // Leaving drops you from the call, so it takes a second tap to confirm.
        if (leave_armed()) {
            pc_leave_voice();
            leave_armed_until = millis();
            buzz(HAPTIC_DOUBLE);
        } else {
            leave_armed_until = millis() + LEAVE_CONFIRM_MS;
            buzz();
        }
        dirty = true;
        break;
    }
    case APP_SETTINGS:
        if (go_next) settings_page = (settings_page + 1) % SETTING_COUNT;
        else if (go_prev) settings_page = (settings_page + SETTING_COUNT - 1) % SETTING_COUNT;
        else if (tap && setting_is_toggle((Setting)settings_page)) setting_cycle((Setting)settings_page);
        else break;
        dirty = true;
        buzz();
        break;
    case APP_PC:
        break;
    }
}

static void poll_touch()
{
    static bool touching, waking;
    static int x0, y0, x1, y1;
    static uint32_t t0;

    int x, y;
    if (read_touch(x, y)) {
        if (!touching) {
            touching = true;
            waking = wake();
            x0 = x;
            y0 = y;
            t0 = millis();
        }
        x1 = x;
        y1 = y;
        last_activity = millis();
        return;
    }
    if (!touching) return;
    touching = false;
    if (waking) return;

    int dx = x1 - x0, dy = y1 - y0;
    bool swipe = abs(dx) >= SWIPE_MIN_PX && abs(dx) > abs(dy);
    bool tap = abs(dx) < TAP_MAX_PX && abs(dy) < TAP_MAX_PX && millis() - t0 < TAP_MAX_MS;
    handle_gesture(x0, y0, dx, dy, tap, swipe);
}

// ---------- Arduino entry points ----------

void setup()
{
    // Avatar messages are ~3 KB and arrive while the screen may be redrawing,
    // so the default few-hundred-byte buffer would overflow and corrupt them.
    Serial.setRxBufferSize(8192);
    Serial.begin(115200);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    has_haptic = haptic.begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL);
    if (has_haptic) {
        haptic.selectLibrary(1);
        haptic.setMode(SensorDRV2605::MODE_INTTRIG);
    }

    knob_config_t cfg = {
        .gpio_encoder_a = PIN_ENCODER_A,
        .gpio_encoder_b = PIN_ENCODER_B,
    };
    knob_handle_t knob = iot_knob_create(&cfg);
    iot_knob_register_cb(knob, KNOB_LEFT, on_knob_left, NULL);
    iot_knob_register_cb(knob, KNOB_RIGHT, on_knob_right, NULL);

    settings_begin();
    prefs.begin("ui");
    uint8_t saved = prefs.getUChar("app", APP_HOME);
    open_app(saved <= APP_LAST ? (App)saved : APP_HOME);

    display_init();
    gfx_begin();
    spotify_begin();
}

void loop()
{
    static uint32_t last_render, last_touch, last_spotify_version = UINT32_MAX;
    static uint32_t last_stats_version, last_mixer_version, last_voice_version, last_notification_version;
    static bool last_connected, last_notifying;
    static UiOverlay overlay;
    static bool last_gauge;
    uint32_t now = millis();
    bool gauge = (int32_t)(now - volume_until) < 0;   // the outer ring, after a dial turn

    pc_poll();
    const PcLink &p = pc();

    if (p.notification_version != last_notification_version) {
        last_notification_version = p.notification_version;
        notification_until = now + NOTIFICATION_MS;
        buzz(HAPTIC_DOUBLE);
    }

    portENTER_CRITICAL(&knob_mux);
    int delta = knob_delta;
    knob_delta = 0;
    portEXIT_CRITICAL(&knob_mux);
    if (delta && !wake()) handle_dial(delta);

    if (now - last_touch >= 15) {
        last_touch = now;
        poll_touch();
    }

    bool connected = pc_connected();
    bool showing_notification = notifying();
    bool redraw = dirty || connected != last_connected || showing_notification != last_notifying;
    last_connected = connected;
    last_notifying = showing_notification;

    switch (app) {
    case APP_HOME:
        if (redraw) home_render(home_selected);
        break;

    case APP_MUSIC: {
        UiOverlay next = {(int32_t)(now - volume_until) < 0, (int32_t)(now - icon_until) < 0 ? icon : ICON_NONE};
        spotify_lock();
        uint32_t version = spotify_state().version;
        bool animating = spotify_state().status == SP_OK && spotify_state().playing;
        spotify_unlock();
        // Redraw on any change, and twice a second while playing to move the progress bar.
        redraw |= version != last_spotify_version || next != overlay || (animating && now - last_render >= 500);
        last_spotify_version = version;
        overlay = next;
        if (redraw) music_render(overlay);
        break;
    }

    case APP_PC:
        redraw |= p.stats_version != last_stats_version;
        if (redraw) pc_render();
        break;

    case APP_MIXER:
        redraw |= p.mixer_version != last_mixer_version || gauge != last_gauge;
        if (redraw) mixer_render(mixer_index(), gauge);
        break;

    case APP_VOICE:
    {
        static bool last_armed;
        bool armed = leave_armed();
        redraw |= p.voice_version != last_voice_version || armed != last_armed;
        last_armed = armed;
        if (redraw) {
            voice_first = min(voice_first, max(0, p.voice.user_count - VOICE_ROWS));
            voice_render(voice_first, voice_action, armed);
        }
        break;
    }

    case APP_SETTINGS:
        redraw |= gauge != last_gauge;
        if (redraw) settings_render(settings_page, gauge);
        break;
    }
    last_gauge = gauge;
    last_stats_version = p.stats_version;
    last_mixer_version = p.mixer_version;
    last_voice_version = p.voice_version;

    if (redraw) {
        if (showing_notification) notification_draw();
        gfx_push();
        dirty = false;
        last_render = now;
    }

    track_pc_lock();
    update_backlight();
    settings_save_if_needed();
    delay(5);
}
