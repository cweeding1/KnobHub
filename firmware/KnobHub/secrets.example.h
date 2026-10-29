#pragma once
// Copy this file to secrets.h in this folder (git ignores it) and fill it in.
// Wi-Fi and Spotify are only needed for the Music app; the rest works over USB.

// The ESP32 only joins 2.4 GHz networks.
#define WIFI_SSID             ""
#define WIFI_PASSWORD         ""

// From your app at https://developer.spotify.com/dashboard
#define SPOTIFY_CLIENT_ID     ""
#define SPOTIFY_CLIENT_SECRET ""

// Written by tools/spotify_auth.py. Leave empty.
#define SPOTIFY_REFRESH_TOKEN ""
