#pragma once
#include <Arduino.h>

#define ART_SIZE 300

enum SpotifyStatus { SP_NEED_SETUP, SP_WIFI_CONNECTING, SP_AUTH_FAILED, SP_NO_PLAYER, SP_OK };

struct PlayerState {
    SpotifyStatus status;
    bool playing;
    uint32_t progress_ms;
    uint32_t progress_at;      // millis() when progress_ms was true
    uint32_t duration_ms;
    int volume;                // 0-100, -1 if unknown
    bool supports_volume;
    char title[96];
    char artist[96];
    bool has_art;
    bool can_save;             // the item can go in Liked Songs and we know whether it's there
    bool saved;                // in Liked Songs (or saved episodes)
    uint32_t version;          // bumps on every change, so the UI knows to redraw
};

// Starts Wi-Fi and the background task that talks to Spotify.
void spotify_begin();

// The state and art may only be read between lock and unlock.
void spotify_lock();
void spotify_unlock();
const PlayerState &spotify_state();
const uint16_t *spotify_art();   // ART_SIZE x ART_SIZE RGB565

// Commands. These update the state immediately and send the request in the background.
void spotify_toggle_play();
void spotify_next();
void spotify_previous();
void spotify_change_volume(int delta);
// Adds the current song to Liked Songs, or removes it.
void spotify_toggle_saved();
