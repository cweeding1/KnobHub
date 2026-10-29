#pragma once
// Each *_render function draws into the canvas; loop() adds any notification and pushes the frame.

enum App { APP_HOME, APP_MUSIC, APP_PC, APP_MIXER, APP_VOICE, APP_SETTINGS };
#define APP_LAST APP_SETTINGS

// ---------- Home ----------

// Apps on the home carousel, in dial order.
extern const App HOME_APPS[];
extern const int HOME_APP_COUNT;

#define HOME_HIT_NONE 99
void home_render(int selected);
// 0 = the centre tile, -1 / +1 = the previous / next tile, HOME_HIT_NONE = nothing.
int home_hit(int x, int y);

// ---------- Music ----------

enum UiIcon { ICON_NONE, ICON_NEXT, ICON_PREV };

struct UiOverlay {
    bool volume;   // show the volume ring and number
    UiIcon icon;   // brief feedback after a skip
    bool operator!=(const UiOverlay &o) const { return volume != o.volume || icon != o.icon; }
};

// Buttons drawn on the left and right edges, used by Music and Mixer.
#define SIDE_BUTTON_R      26
#define SIDE_BUTTON_PREV_X 70
#define SIDE_BUTTON_NEXT_X 290
// Taps left of / right of these x positions count as the side buttons.
#define SIDE_ZONE_PREV_MAX 120
#define SIDE_ZONE_NEXT_MIN 240

void music_render(const UiOverlay &overlay);

// ---------- PC ----------

void pc_render();
// index into pc().sessions
// gauge shows the volume ring, for a moment after the dial turns.
void mixer_render(int selected, bool gauge);
// The action bar along the bottom shows one of these at a time; the dial picks it.
enum VoiceAction { VOICE_MUTE, VOICE_LEAVE, VOICE_DEAFEN, VOICE_ACTION_COUNT };
// first is the index of the first member shown; leave_armed shows the
// "tap again to leave" state of the leave action.
void voice_render(int first, VoiceAction action, bool leave_armed);
#define VOICE_ROWS 4

// ---------- Settings ----------

// page is a Setting from settings.h
// gauge shows the value's ring, for a moment after the dial turns.
void settings_render(int page, bool gauge);

// ---------- Notifications ----------

void notification_draw();
