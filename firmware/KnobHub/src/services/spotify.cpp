#include "spotify.h"
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <Preferences.h>
#include <base64.h>
// secrets.h holds your own Wi-Fi and Spotify keys and isn't in git. Without
// it the build still works and the Music app shows "Setup needed".
#if __has_include("../../secrets.h")
#include "../../secrets.h"
#else
#include "../../secrets.example.h"
#endif
#include "../ui/text.h"

#define SPOTIFY_DEBUG 0   // log API calls to serial; off so they never interleave with PC link messages

#define JPEG_BUF_SIZE (512 * 1024)
#define HOLD_MS 3000   // how long an optimistic UI change wins over polled state
#define RESTART_THRESHOLD_MS 3000
#define SAVED_CHECK_MS 15000   // re-check Liked Songs, in case it changed in the app
#define FAST_POLL_MS   250     // poll interval while waiting for a skip to land
#define FAST_POLL_FOR  3000

static SemaphoreHandle_t mutex;
static PlayerState state;
static uint16_t *art_front, *art_back;
static uint8_t *jpeg_buf;

// Pending commands, guarded by mutex. Volume is a value rather than a queue
// so a fast spin of the dial collapses into a few requests.
static int want_play = -1;        // 1 play, 0 pause, -1 nothing
static int want_skip = 0;         // 1 next, -1 previous
static bool want_restart = false; // seek to the start of the current song
static int want_volume = -1;
static int want_saved = -1;       // 1 save, 0 remove, -1 nothing
static uint32_t volume_changed_at, volume_sent_at;
static uint32_t hold_playing_until, hold_volume_until, hold_saved_until;
static uint32_t saved_checked_at;
static bool saved_check_due;

static String access_token, refresh_token;
static uint32_t token_expires_at;
static uint32_t backoff_until;
static char track_key[128];
static char library_uri[64];      // what Liked Songs knows the current item as
static uint32_t fast_poll_until;  // poll quickly until the track changes

static Preferences prefs;
static NetworkClientSecure api_client, img_client;
static HTTPClient api_http, img_http;
static JPEGDEC jpeg;

static bool before(uint32_t t) { return (int32_t)(millis() - t) < 0; }

void spotify_lock()   { xSemaphoreTake(mutex, portMAX_DELAY); }
void spotify_unlock() { xSemaphoreGive(mutex); }
const PlayerState &spotify_state() { return state; }
const uint16_t *spotify_art()      { return art_front; }

// ---------- HTTP ----------

// Lets HTTPClient::writeToStream fill a plain buffer.
class BufferStream : public Stream {
public:
    BufferStream(uint8_t *buf, size_t cap) : buf(buf), cap(cap) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *data, size_t n) override
    {
        size_t k = min(n, cap - len);
        memcpy(buf + len, data, k);
        len += k;
        return k;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    size_t len = 0;
private:
    uint8_t *buf;
    size_t cap;
};

static int api(const char *method, const String &path, String *body = nullptr)
{
    static const char *keep_headers[] = {"Retry-After"};
    api_http.begin(api_client, "https://api.spotify.com/v1" + path);
    api_http.setReuse(true);
    api_http.setTimeout(6000);
    api_http.collectHeaders(keep_headers, 1);
    api_http.addHeader("Authorization", "Bearer " + access_token);
    int code;
    if (strcmp(method, "GET") == 0) {
        code = api_http.GET();
    } else {
        // Spotify rejects bodiless PUT/POST without this, and HTTPClient omits it.
        api_http.addHeader("Content-Length", "0");
        code = api_http.sendRequest(method);
    }
    if (code == 200 && body) *body = api_http.getString();
    if (code == 401) access_token = "";
    if (code == 429) backoff_until = millis() + 1000 * max(1L, api_http.header("Retry-After").toInt());
    api_http.end();
    if (SPOTIFY_DEBUG) Serial.printf("%s %s -> %d\n", method, path.c_str(), code);
    return code;
}

enum TokenResult { TOKEN_OK, TOKEN_REJECTED, TOKEN_NETWORK_ERROR };

static TokenResult refresh_access_token()
{
    NetworkClientSecure client;
    client.useBuiltinCACertBundle();
    HTTPClient http;
    http.begin(client, "https://accounts.spotify.com/api/token");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.addHeader("Authorization", "Basic " + base64::encode(String(SPOTIFY_CLIENT_ID) + ":" + SPOTIFY_CLIENT_SECRET));
    int code = http.POST("grant_type=refresh_token&refresh_token=" + refresh_token);
    String body = code > 0 ? http.getString() : "";
    http.end();
    if (SPOTIFY_DEBUG) Serial.printf("token refresh -> %d\n", code);
    if (code < 0 || code >= 500) return TOKEN_NETWORK_ERROR;
    if (code != 200) {
        if (SPOTIFY_DEBUG) Serial.println(body);
        return TOKEN_REJECTED;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) return TOKEN_NETWORK_ERROR;
    access_token = doc["access_token"].as<String>();
    token_expires_at = millis() + (doc["expires_in"] | 3600) * 1000UL - 60000;
    // Spotify sometimes rotates the refresh token. Keep the newest one, tagged
    // with the token from secrets.h so re-running spotify_auth.py still wins.
    if (doc["refresh_token"].is<const char *>()) {
        refresh_token = doc["refresh_token"].as<String>();
        prefs.putString("rt", refresh_token);
        prefs.putString("rt_seed", SPOTIFY_REFRESH_TOKEN);
    }
    return TOKEN_OK;
}

// ---------- Album art ----------

static int art_offset_x, art_offset_y;

static int jpeg_draw(JPEGDRAW *d)
{
    for (int row = 0; row < d->iHeight; row++) {
        int y = d->y + row - art_offset_y;
        if (y < 0 || y >= ART_SIZE) continue;
        for (int col = 0; col < d->iWidth; col++) {
            int x = d->x + col - art_offset_x;
            if (x < 0 || x >= ART_SIZE) continue;
            art_back[y * ART_SIZE + x] = d->pPixels[row * d->iWidth + col];
        }
    }
    return 1;
}

// Downloads and decodes a JPEG into art_back, centred and cropped to ART_SIZE.
static bool fetch_art(const char *url)
{
    // Kept alive between songs: a fresh TLS handshake costs about a second.
    img_http.begin(img_client, url);
    img_http.setReuse(true);
    img_http.setTimeout(8000);
    int code = img_http.GET();
    BufferStream out(jpeg_buf, JPEG_BUF_SIZE);
    if (code == 200) img_http.writeToStream(&out);
    img_http.end();
    if (code != 200 || out.len == 0) return false;

    if (!jpeg.openRAM(jpeg_buf, out.len, jpeg_draw)) return false;
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    int w = jpeg.getWidth(), h = jpeg.getHeight();
    int opts = 0;
    if (w >= ART_SIZE * 4)      { opts = JPEG_SCALE_QUARTER; w /= 4; h /= 4; }
    else if (w >= ART_SIZE * 2) { opts = JPEG_SCALE_HALF; w /= 2; h /= 2; }
    art_offset_x = (w - ART_SIZE) / 2;
    art_offset_y = (h - ART_SIZE) / 2;
    memset(art_back, 0, ART_SIZE * ART_SIZE * sizeof(uint16_t));
    bool ok = jpeg.decode(0, 0, opts) == 1;
    jpeg.close();
    return ok;
}

// ---------- Polling ----------

static void set_status(SpotifyStatus s)
{
    spotify_lock();
    if (state.status != s) {
        state.status = s;
        state.version++;
    }
    spotify_unlock();
}

static void check_saved();

// Fetches the player state. Returns how long to wait before the next poll.
static uint32_t poll_player()
{
    String body;
    int code = api("GET", "/me/player?market=from_token&additional_types=episode", &body);
    if (code == 204) {
        set_status(SP_NO_PLAYER);
        track_key[0] = 0;
        return 5000;
    }
    if (code != 200) return 3000;

    JsonDocument filter;
    filter["is_playing"] = true;
    filter["progress_ms"] = true;
    filter["device"]["volume_percent"] = true;
    filter["device"]["supports_volume"] = true;
    JsonObject f_item = filter["item"].to<JsonObject>();
    f_item["uri"] = true;
    f_item["linked_from"]["uri"] = true;
    f_item["name"] = true;
    f_item["duration_ms"] = true;
    f_item["artists"][0]["name"] = true;
    f_item["album"]["images"][0]["url"] = true;
    f_item["album"]["images"][0]["width"] = true;
    f_item["images"][0]["url"] = true;
    f_item["images"][0]["width"] = true;
    f_item["show"]["name"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return 3000;
    JsonObject item = doc["item"];
    if (item.isNull()) {
        // Ads and private sessions report a player but no item.
        set_status(SP_NO_PLAYER);
        return 5000;
    }

    String artist;
    JsonArray artists = item["artists"];
    for (JsonObject a : artists) {
        if (artist.length()) artist += ", ";
        artist += a["name"].as<const char *>();
    }
    if (artist.isEmpty()) artist = item["show"]["name"] | "";

    // Prefer the smallest image that still covers ART_SIZE.
    JsonArray images = item["album"]["images"];
    if (images.isNull() || images.size() == 0) images = item["images"];
    const char *art_url = nullptr;
    int best_w = 0;
    for (JsonObject img : images) {
        int w = img["width"] | 0;
        bool better = !art_url || (w >= ART_SIZE && (best_w < ART_SIZE || w < best_w)) || (best_w < ART_SIZE && w > best_w);
        if (better) {
            art_url = img["url"];
            best_w = w;
        }
    }

    const char *key = item["uri"] | "";
    if (!*key) key = item["name"] | "";   // local files have no URI
    bool new_track = strcmp(key, track_key) != 0;
    bool playing = doc["is_playing"] | false;
    uint32_t progress = doc["progress_ms"] | 0;
    uint32_t duration = item["duration_ms"] | 0;

    spotify_lock();
    state.status = SP_OK;
    if (!before(hold_playing_until)) state.playing = playing;
    state.progress_ms = progress;
    state.progress_at = millis();
    state.duration_ms = duration;
    if (!before(hold_volume_until)) state.volume = doc["device"]["volume_percent"] | -1;
    state.supports_volume = doc["device"]["supports_volume"] | true;
    to_ascii(item["name"] | "", state.title, sizeof(state.title));
    to_ascii(artist.c_str(), state.artist, sizeof(state.artist));
    state.version++;
    spotify_unlock();

    if (new_track) {
        strlcpy(track_key, key, sizeof(track_key));
        // Playlists often play a regional copy of a song ("relinking"); Liked
        // Songs only knows the original.
        strlcpy(library_uri, item["linked_from"]["uri"] | key, sizeof(library_uri));
        fast_poll_until = 0;
        spotify_lock();
        // Hidden until the check says whether it's saved.
        state.can_save = state.saved = false;
        hold_saved_until = 0;
        spotify_unlock();
        check_saved();   // before the art: it's quick, and the art can take a moment
        bool ok = art_url && fetch_art(art_url);
        spotify_lock();
        if (ok) std::swap(art_front, art_back);
        state.has_art = ok;
        state.version++;
        spotify_unlock();
    }

    if (before(fast_poll_until)) return FAST_POLL_MS;
    if (!playing) return 3000;
    // Poll right after the song ends so the next one shows up promptly.
    uint32_t remaining = duration > progress ? duration - progress : 0;
    // About to end: once it has, poll quickly until the next song shows up.
    if (remaining < 1000) fast_poll_until = millis() + remaining + FAST_POLL_FOR;
    return min(2000UL, (unsigned long)remaining + 500);
}

// ---------- Liked Songs ----------

// Only tracks and episodes can be saved; local files have no Spotify URI.
static bool saveable(const char *uri)
{
    return !strncmp(uri, "spotify:track:", 14) || !strncmp(uri, "spotify:episode:", 16);
}

static String library_path(const char *uri)
{
    String s = uri;
    s.replace(":", "%3A");
    return "/me/library?uris=" + s;
}

static void set_saved_state(bool can_save, bool saved)
{
    spotify_lock();
    if (!before(hold_saved_until) && (state.can_save != can_save || state.saved != saved)) {
        state.can_save = can_save;
        state.saved = saved;
        state.version++;
    }
    spotify_unlock();
}

static void check_saved()
{
    saved_check_due = false;
    saved_checked_at = millis();
    if (!saveable(library_uri)) {
        set_saved_state(false, false);
        return;
    }
    String path = library_path(library_uri);
    path.replace("/me/library?", "/me/library/contains?");
    String body;
    int code = api("GET", path, &body);
    if (code == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, body)) set_saved_state(true, doc[0] | false);
    } else if (code == 400 || code == 403) {
        // 403: the login predates the library permission; run spotify_auth.py again.
        set_saved_state(false, false);
    }
}

// ---------- Background task ----------

static void net_task(void *)
{
    uint32_t next_poll = 0;
    for (;;) {
        delay(20);
        if (WiFi.status() != WL_CONNECTED) {
            set_status(SP_WIFI_CONNECTING);
            next_poll = 0;
            continue;
        }
        if (before(backoff_until)) continue;

        if (access_token.isEmpty() || !before(token_expires_at)) {
            TokenResult r = refresh_access_token();
            if (r != TOKEN_OK) {
                if (r == TOKEN_REJECTED) set_status(SP_AUTH_FAILED);
                backoff_until = millis() + (r == TOKEN_REJECTED ? 60000 : 3000);
                continue;
            }
        }

        spotify_lock();
        int play = want_play, skip = want_skip, volume = -1, saved = want_saved;
        bool restart = want_restart;
        want_saved = -1;
        want_play = -1;
        want_skip = 0;
        want_restart = false;
        // Send volume once the dial pauses briefly, or every 400 ms while it keeps turning.
        if (want_volume >= 0 && (millis() - volume_changed_at >= 150 || millis() - volume_sent_at >= 400)) {
            volume = want_volume;
            want_volume = -1;
            volume_sent_at = millis();
        }
        spotify_unlock();

        if (play >= 0) {
            api("PUT", play ? "/me/player/play" : "/me/player/pause");
            next_poll = millis() + 500;
        }
        if (skip) {
            api("POST", skip > 0 ? "/me/player/next" : "/me/player/previous");
            // Spotify takes a moment to switch, so poll quickly until the new track shows.
            next_poll = millis() + FAST_POLL_MS;
            fast_poll_until = millis() + FAST_POLL_FOR;
        }
        if (restart) {
            api("PUT", "/me/player/seek?position_ms=0");
            next_poll = millis() + 500;
        }
        if (volume >= 0) {
            int code = api("PUT", "/me/player/volume?volume_percent=" + String(volume));
            if (code == 403) {
                spotify_lock();
                state.supports_volume = false;
                state.version++;
                spotify_unlock();
            }
        }
        if (saved >= 0 && saveable(library_uri)) {
            // On failure, the next check puts the heart back how Spotify has it.
            if (api(saved ? "PUT" : "DELETE", library_path(library_uri)) != 200) hold_saved_until = 0;
            saved_check_due = true;
            saved_checked_at = millis();
        }
        if (!before(next_poll)) next_poll = millis() + poll_player();
        if (state.status == SP_OK && track_key[0] && !before(hold_saved_until) &&
            (saved_check_due || millis() - saved_checked_at >= SAVED_CHECK_MS))
            check_saved();
    }
}

void spotify_begin()
{
    mutex = xSemaphoreCreateMutex();
    art_front = (uint16_t *)ps_malloc(ART_SIZE * ART_SIZE * sizeof(uint16_t));
    art_back = (uint16_t *)ps_malloc(ART_SIZE * ART_SIZE * sizeof(uint16_t));
    jpeg_buf = (uint8_t *)ps_malloc(JPEG_BUF_SIZE);
    state.volume = -1;
    state.supports_volume = true;

    if (!*WIFI_SSID || !*SPOTIFY_CLIENT_ID || !*SPOTIFY_CLIENT_SECRET || !*SPOTIFY_REFRESH_TOKEN) {
        state.status = SP_NEED_SETUP;
        return;
    }
    state.status = SP_WIFI_CONNECTING;

    prefs.begin("spotify");
    refresh_token = SPOTIFY_REFRESH_TOKEN;
    if (prefs.getString("rt_seed", "") == SPOTIFY_REFRESH_TOKEN) refresh_token = prefs.getString("rt", refresh_token);

    api_client.useBuiltinCACertBundle();
    img_client.useBuiltinCACertBundle();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    xTaskCreatePinnedToCore(net_task, "spotify", 16384, nullptr, 1, nullptr, 0);
}

// ---------- Commands ----------

static uint32_t current_progress()
{
    uint32_t p = state.progress_ms + (state.playing ? millis() - state.progress_at : 0);
    return min(p, state.duration_ms);
}

void spotify_toggle_play()
{
    spotify_lock();
    state.progress_ms = current_progress();
    state.progress_at = millis();
    state.playing = !state.playing;
    want_play = state.playing;
    hold_playing_until = millis() + HOLD_MS;
    state.version++;
    spotify_unlock();
}

static void skip(int dir)
{
    spotify_lock();
    // Like most players, "previous" restarts the song unless it has only just begun.
    if (dir < 0 && current_progress() > RESTART_THRESHOLD_MS) want_restart = true;
    else want_skip = dir;
    state.progress_ms = 0;
    state.progress_at = millis();
    state.version++;
    spotify_unlock();
}
void spotify_next()     { skip(1); }
void spotify_previous() { skip(-1); }

void spotify_toggle_saved()
{
    spotify_lock();
    if (state.can_save) {
        state.saved = !state.saved;
        want_saved = state.saved;
        hold_saved_until = millis() + HOLD_MS;
        state.version++;
    }
    spotify_unlock();
}

void spotify_change_volume(int delta)
{
    spotify_lock();
    if (state.volume >= 0 && state.supports_volume) {
        state.volume = constrain(state.volume + delta, 0, 100);
        want_volume = state.volume;
        volume_changed_at = millis();
        hold_volume_until = millis() + HOLD_MS;
        state.version++;
    }
    spotify_unlock();
}
