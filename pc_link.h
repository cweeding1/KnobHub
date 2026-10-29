#pragma once
// Everything pc_agent.py sends over USB serial, plus the commands sent back.
// Only touched from loop(), so no locking.
#include <Arduino.h>

struct PcStats {
    char host[32];
    float cpu, gpu;                 // percent
    float ram_used, ram_total;      // GB
    float vram_used, vram_total;    // GB
    float net_down, net_up;         // bytes per second
    float cpu_temp, gpu_temp;       // Celsius, NAN if unknown
    bool locked;                    // Windows lock screen is up
};

#define MAX_SESSIONS 12
struct MixerSession {
    char id[40];     // "master" or the app's exe name
    char name[32];
    int volume;      // 0-100
    bool muted;
};

#define MAX_VOICE_USERS 16
struct VoiceUser {
    char id[24];
    char name[32];
    bool speaking, muted, deaf;
};

#define AVATAR_SIZE 32   // must match AVATAR_SIZE in pc_agent.py

enum VoiceStatus { VOICE_STARTING, VOICE_NO_CONFIG, VOICE_NO_DISCORD, VOICE_APPROVE, VOICE_DENIED, VOICE_ERROR, VOICE_OK };

struct VoiceState {
    VoiceStatus status;
    bool in_channel;
    bool self_mute, self_deaf;   // your own mic and headphones
    char channel[48];
    char guild[48];
    int user_count;
    VoiceUser users[MAX_VOICE_USERS];
};

struct Notification {
    char title[64];
    char body[160];
};

struct PcLink {
    PcStats stats;
    MixerSession sessions[MAX_SESSIONS];
    int session_count;
    VoiceState voice;
    Notification notification;
    // Each bumps when its part changes.
    uint32_t stats_version, mixer_version, voice_version, notification_version;
};

// Reads pending serial input and flushes queued commands. Call often from loop().
void pc_poll();
const PcLink &pc();
// True while messages keep arriving, i.e. plugged into a PC running the agent.
bool pc_connected();
// A Discord member's avatar (AVATAR_SIZE square, RGB565), or nullptr if it hasn't arrived.
const uint16_t *pc_avatar(const char *user_id);

// Updates the local copy immediately and sends the change to the PC.
void pc_set_volume(const char *id, int volume);
void pc_set_mute(const char *id, bool muted);
// Asks Discord to leave the current voice channel.
void pc_leave_voice();
// Mutes / deafens you in Discord. Shown locally right away.
void pc_set_voice_mute(bool muted);
void pc_set_voice_deaf(bool deaf);
