"""One-time Spotify login for the knob.

Reads the client ID and secret from secrets.h, opens a browser to log in to
Spotify, and writes the resulting refresh token back into secrets.h.

In the Spotify developer dashboard, the app's redirect URI must be exactly:
    http://127.0.0.1:8888/callback
"""
import base64
import http.server
import json
import pathlib
import re
import secrets
import sys
import urllib.parse
import urllib.request
import webbrowser

REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPES = ("user-read-playback-state user-modify-playback-state user-read-currently-playing "
          "user-library-read user-library-modify")
SECRETS = pathlib.Path(__file__).resolve().parent.parent / "firmware" / "KnobHub" / "secrets.h"


def read_define(text, name):
    m = re.search(rf'#define\s+{name}\s+"([^"]*)"', text)
    return m.group(1) if m else ""


def main():
    if not SECRETS.exists():
        sys.exit("Copy firmware/KnobHub/secrets.example.h to secrets.h (same folder) and fill it in first.")
    text = SECRETS.read_text()
    client_id = read_define(text, "SPOTIFY_CLIENT_ID")
    client_secret = read_define(text, "SPOTIFY_CLIENT_SECRET")
    if not client_id or not client_secret:
        sys.exit("Fill in SPOTIFY_CLIENT_ID and SPOTIFY_CLIENT_SECRET in secrets.h first.")

    state = secrets.token_urlsafe(16)
    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            if url.path != "/callback":
                self.send_response(404)
                self.end_headers()
                return
            query = urllib.parse.parse_qs(url.query)
            if query.get("state", [""])[0] != state:
                result["error"] = "state mismatch"
            elif "error" in query:
                result["error"] = query["error"][0]
            else:
                result["code"] = query.get("code", [""])[0]
            ok = "code" in result
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            msg = "Done. You can close this tab." if ok else f"Login failed: {result.get('error')}"
            self.wfile.write(f"<h2>{msg}</h2>".encode())

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 8888), Handler)
    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "scope": SCOPES,
        "state": state,
    })
    print("Opening Spotify login in your browser. If it doesn't open, visit:")
    print(auth_url, flush=True)
    webbrowser.open(auth_url)
    while not result:
        server.handle_request()
    server.server_close()

    if "code" not in result:
        sys.exit(f"Login failed: {result['error']}")

    basic = base64.b64encode(f"{client_id}:{client_secret}".encode()).decode()
    req = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=urllib.parse.urlencode({
            "grant_type": "authorization_code",
            "code": result["code"],
            "redirect_uri": REDIRECT_URI,
        }).encode(),
        headers={"Authorization": f"Basic {basic}",
                 "Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req) as resp:
            tokens = json.load(resp)
    except urllib.error.HTTPError as e:
        sys.exit(f"Token exchange failed: {e.code} {e.read().decode(errors='replace')}")

    refresh = tokens["refresh_token"]
    text = re.sub(r'(#define\s+SPOTIFY_REFRESH_TOKEN\s+)"[^"]*"', lambda m: f'{m.group(1)}"{refresh}"', text)
    SECRETS.write_text(text)
    print("Success: refresh token saved to secrets.h")


if __name__ == "__main__":
    main()
