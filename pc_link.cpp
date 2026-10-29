#include "pc_link.h"
#include "text.h"
#include <ArduinoJson.h>
#include "mbedtls/base64.h"

#define STALE_MS         3500
#define HOLD_MS          1500   // local mixer changes win over the PC's copy this long
#define VOLUME_SEND_MS   60     // at most one volume command per this interval
#define AVATAR_SLOTS     MAX_VOICE_USERS

static PcLink state;
static uint32_t last_message_at;
static bool have_message;
static char line[4096];   // fits an avatar message
static size_t line_len;

struct AvatarSlot {
    char user_id[24];
    uint32_t used_at;   // for evicting the least recently used
    uint16_t px[AVATAR_SIZE * AVATAR_SIZE];
};
static AvatarSlot *avatars;   // in PSRAM

static char hold_id[40];
static uint32_t hold_until;
static char pending_volume_id[40];
static int pending_volume = -1;
static uint32_t volume_sent_at;
static uint32_t voice_hold_until;   // like hold_until, for your own mute/deafen

static void copy_ascii(JsonVariantConst v, char *out, size_t n)
{
    to_ascii(v | "", out, n);
}

static float temp_or_nan(JsonVariantConst v)
{
    return v.is<float>() ? v.as<float>() : NAN;
}

static MixerSession *find_session(const char *id)
{
    for (int i = 0; i < state.session_count; i++)
        if (strcmp(state.sessions[i].id, id) == 0) return &state.sessions[i];
    return nullptr;
}

static void parse_stats(JsonDocument &doc)
{
    PcStats &s = state.stats;
    copy_ascii(doc["host"], s.host, sizeof(s.host));
    s.cpu = doc["cpu"] | 0.0f;
    s.gpu = doc["gpu"] | 0.0f;
    s.ram_used = doc["ram"][0] | 0.0f;
    s.ram_total = doc["ram"][1] | 0.0f;
    s.vram_used = doc["vram"][0] | 0.0f;
    s.vram_total = doc["vram"][1] | 0.0f;
    s.net_down = doc["down"] | 0.0f;
    s.net_up = doc["up"] | 0.0f;
    s.cpu_temp = temp_or_nan(doc["ct"]);
    s.gpu_temp = temp_or_nan(doc["gt"]);
    s.locked = (doc["lk"] | 0) != 0;
    state.stats_version++;
}

static void parse_mixer(JsonDocument &doc)
{
    // Keep the session the user is adjusting as it is locally until the PC catches up.
    MixerSession held = {};
    bool holding = (int32_t)(millis() - hold_until) < 0;
    if (holding) {
        MixerSession *h = find_session(hold_id);
        if (h) held = *h;
        else holding = false;
    }

    int n = 0;
    for (JsonObjectConst s : doc["s"].as<JsonArrayConst>()) {
        if (n == MAX_SESSIONS) break;
        MixerSession &m = state.sessions[n++];
        strlcpy(m.id, s["id"] | "", sizeof(m.id));
        copy_ascii(s["n"], m.name, sizeof(m.name));
        m.volume = s["v"] | 0;
        m.muted = (s["m"] | 0) != 0;
        if (holding && strcmp(m.id, held.id) == 0) m = held;
    }
    state.session_count = n;
    state.mixer_version++;
}

static VoiceStatus voice_status(const char *s)
{
    if (!strcmp(s, "ok"))         return VOICE_OK;
    if (!strcmp(s, "no_config"))  return VOICE_NO_CONFIG;
    if (!strcmp(s, "no_discord")) return VOICE_NO_DISCORD;
    if (!strcmp(s, "approve"))    return VOICE_APPROVE;
    if (!strcmp(s, "denied"))     return VOICE_DENIED;
    if (!strcmp(s, "error"))      return VOICE_ERROR;
    return VOICE_STARTING;
}

static void parse_voice(JsonDocument &doc)
{
    VoiceState &v = state.voice;
    v.status = voice_status(doc["st"] | "");
    if ((int32_t)(millis() - voice_hold_until) >= 0) {
        v.self_mute = (doc["sm"] | 0) != 0;
        v.self_deaf = (doc["sd"] | 0) != 0;
    }
    v.in_channel = doc["ch"].is<const char *>();
    copy_ascii(doc["ch"], v.channel, sizeof(v.channel));
    copy_ascii(doc["g"], v.guild, sizeof(v.guild));
    int n = 0;
    for (JsonObjectConst u : doc["u"].as<JsonArrayConst>()) {
        if (n == MAX_VOICE_USERS) break;
        VoiceUser &vu = v.users[n++];
        strlcpy(vu.id, u["id"] | "", sizeof(vu.id));
        copy_ascii(u["n"], vu.name, sizeof(vu.name));
        vu.speaking = (u["s"] | 0) != 0;
        vu.muted = (u["m"] | 0) != 0;
        vu.deaf = (u["d"] | 0) != 0;
    }
    v.user_count = n;
    state.voice_version++;
}

static void parse_notification(JsonDocument &doc)
{
    copy_ascii(doc["title"], state.notification.title, sizeof(state.notification.title));
    copy_ascii(doc["body"], state.notification.body, sizeof(state.notification.body));
    state.notification_version++;
}

static AvatarSlot *find_avatar(const char *user_id)
{
    for (int i = 0; i < AVATAR_SLOTS; i++)
        if (avatars[i].user_id[0] && strcmp(avatars[i].user_id, user_id) == 0) return &avatars[i];
    return nullptr;
}

static void parse_avatar(JsonDocument &doc)
{
    if (!avatars) avatars = (AvatarSlot *)ps_calloc(AVATAR_SLOTS, sizeof(AvatarSlot));
    if (!avatars) return;
    const char *id = doc["id"] | "";
    const char *b64 = doc["px"] | "";
    AvatarSlot *slot = find_avatar(id);
    if (!slot) {
        slot = &avatars[0];
        for (int i = 1; i < AVATAR_SLOTS; i++)
            if (avatars[i].used_at < slot->used_at) slot = &avatars[i];
    }
    size_t out_len = 0;
    if (mbedtls_base64_decode((uint8_t *)slot->px, sizeof(slot->px), &out_len, (const uint8_t *)b64, strlen(b64)) != 0 ||
        out_len != sizeof(slot->px)) {
        slot->user_id[0] = 0;
        return;
    }
    strlcpy(slot->user_id, id, sizeof(slot->user_id));
    slot->used_at = millis();
    state.voice_version++;
}

const uint16_t *pc_avatar(const char *user_id)
{
    if (!avatars) return nullptr;
    AvatarSlot *slot = find_avatar(user_id);
    if (!slot) return nullptr;
    slot->used_at = millis();
    return slot->px;
}

static void parse_line()
{
    JsonDocument doc;
    if (deserializeJson(doc, line, line_len)) return;
    const char *type = doc["t"] | "";
    if (!strcmp(type, "stats"))       parse_stats(doc);
    else if (!strcmp(type, "mixer"))  parse_mixer(doc);
    else if (!strcmp(type, "voice"))  parse_voice(doc);
    else if (!strcmp(type, "avatar")) parse_avatar(doc);
    else if (!strcmp(type, "notify")) parse_notification(doc);
    else return;
    last_message_at = millis();
    have_message = true;
}

static void send_command(JsonDocument &doc)
{
    serializeJson(doc, Serial);
    Serial.print('\n');
}

void pc_poll()
{
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            if (line_len && line[0] == '{') parse_line();
            line_len = 0;
        } else if (line_len < sizeof(line) - 1) {
            line[line_len++] = c;
        }
    }

    if (pending_volume >= 0 && millis() - volume_sent_at >= VOLUME_SEND_MS) {
        JsonDocument doc;
        doc["cmd"] = "vol";
        doc["id"] = pending_volume_id;
        doc["v"] = pending_volume;
        send_command(doc);
        pending_volume = -1;
        volume_sent_at = millis();
    }
}

const PcLink &pc() { return state; }

bool pc_connected()
{
    return have_message && millis() - last_message_at < STALE_MS;
}

static void hold(const char *id)
{
    strlcpy(hold_id, id, sizeof(hold_id));
    hold_until = millis() + HOLD_MS;
    state.mixer_version++;
}

void pc_set_volume(const char *id, int volume)
{
    MixerSession *s = find_session(id);
    if (!s) return;
    s->volume = constrain(volume, 0, 100);
    strlcpy(pending_volume_id, id, sizeof(pending_volume_id));
    pending_volume = s->volume;
    hold(id);
}

void pc_leave_voice()
{
    JsonDocument doc;
    doc["cmd"] = "leave";
    send_command(doc);
}

void pc_set_voice_mute(bool muted)
{
    state.voice.self_mute = muted;
    // Unmuting also undeafens, as in Discord.
    if (!muted) state.voice.self_deaf = false;
    voice_hold_until = millis() + HOLD_MS;
    state.voice_version++;
    JsonDocument doc;
    doc["cmd"] = "dmute";
    doc["m"] = muted ? 1 : 0;
    send_command(doc);
}

void pc_set_voice_deaf(bool deaf)
{
    state.voice.self_deaf = deaf;
    voice_hold_until = millis() + HOLD_MS;
    state.voice_version++;
    JsonDocument doc;
    doc["cmd"] = "ddeaf";
    doc["d"] = deaf ? 1 : 0;
    send_command(doc);
}

void pc_set_mute(const char *id, bool muted)
{
    MixerSession *s = find_session(id);
    if (!s) return;
    s->muted = muted;
    JsonDocument doc;
    doc["cmd"] = "mute";
    doc["id"] = id;
    doc["m"] = muted ? 1 : 0;
    send_command(doc);
    hold(id);
}
