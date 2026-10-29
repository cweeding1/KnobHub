"""Minimal client for the Discord desktop app's local RPC interface.

Tracks the voice channel you're in, who is in it and who is speaking, your
own mute/deafen state, and forwards message notifications. Uses your own
Discord developer application (see discord.json); the first run asks you to
approve it inside Discord.
"""
import json
import logging
import msvcrt
import pathlib
import queue
import struct
import threading
import time
import urllib.parse
import urllib.request
import uuid

import win32pipe

OP_HANDSHAKE, OP_FRAME, OP_CLOSE = 0, 1, 2
SCOPES = ["rpc", "rpc.voice.read", "rpc.voice.write", "rpc.notifications.read"]
REDIRECT_URI = "http://127.0.0.1"
IDLE_S = 0.02   # how often the main loop checks for events and queued commands
VOICE_EVENTS = ["VOICE_STATE_CREATE", "VOICE_STATE_UPDATE", "VOICE_STATE_DELETE", "SPEAKING_START", "SPEAKING_STOP"]

HERE = pathlib.Path(__file__).resolve().parent
CONFIG_FILE = HERE / "discord.json"
TOKEN_FILE = HERE / "discord_token.json"

log = logging.getLogger("discord")


class RpcError(Exception):
    pass


class DiscordVoice:
    """Runs in a background thread. Read the current state with snapshot()."""

    def __init__(self, on_notify):
        self.on_notify = on_notify
        self.lock = threading.Lock()
        self.status = "starting"   # no_config | no_discord | approve | denied | error | ok
        self.channel = None        # {"name", "guild", "users": {user_id: {...}}}
        self.version = 0
        self.pipe = None
        self.channel_id = None
        self.self_mute = self.self_deaf = False
        self.commands = queue.Queue()   # (cmd, args, log message), sent by the session thread
        threading.Thread(target=self._run, daemon=True).start()

    def snapshot(self):
        """Returns (status, channel, (self_mute, self_deaf), version)."""
        with self.lock:
            channel = None
            if self.channel:
                channel = {
                    "name": self.channel["name"],
                    "guild": self.channel["guild"],
                    "users": sorted(self.channel["users"].values(), key=lambda u: u["name"].lower()),
                }
            return self.status, channel, (self.self_mute, self.self_deaf), self.version

    # ---------- state helpers ----------

    def _set(self, status=None, channel=...):
        with self.lock:
            if status is not None:
                self.status = status
            if channel is not ...:
                self.channel = channel
            self.version += 1

    def _upsert_user(self, state):
        user = state.get("user", {})
        vs = state.get("voice_state", {})
        uid = user.get("id")
        if not uid:
            return
        with self.lock:
            if not self.channel:
                return
            old = self.channel["users"].get(uid, {})
            self.channel["users"][uid] = {
                "id": uid,
                "avatar": user.get("avatar"),
                "name": state.get("nick") or user.get("global_name") or user.get("username") or "?",
                "muted": bool(vs.get("mute") or vs.get("self_mute") or state.get("mute")),
                "deaf": bool(vs.get("deaf") or vs.get("self_deaf")),
                "speaking": old.get("speaking", False),
            }
            self.version += 1

    def _set_self(self, settings):
        with self.lock:
            self.self_mute = bool(settings.get("mute"))
            self.self_deaf = bool(settings.get("deaf"))
            self.version += 1

    def _set_speaking(self, uid, speaking):
        with self.lock:
            if self.channel and uid in self.channel["users"]:
                self.channel["users"][uid]["speaking"] = speaking
                self.version += 1

    # ---------- pipe protocol ----------

    def _send(self, op, payload):
        data = json.dumps(payload).encode()
        self.pipe.write(struct.pack("<II", op, len(data)) + data)

    def _recv(self):
        header = self.pipe.read(8)
        if len(header) < 8:
            raise ConnectionError("pipe closed")
        op, length = struct.unpack("<II", header)
        payload = json.loads(self.pipe.read(length))
        if op == OP_CLOSE:
            raise ConnectionError(payload.get("message", "closed by Discord"))
        return payload

    def _call(self, cmd, args=None, evt=None):
        """Sends a command and waits for its reply, handling events that arrive meanwhile."""
        nonce = str(uuid.uuid4())
        msg = {"cmd": cmd, "args": args or {}, "nonce": nonce}
        if evt:
            msg["evt"] = evt
        self._send(OP_FRAME, msg)
        while True:
            reply = self._recv()
            if reply.get("nonce") == nonce:
                if reply.get("evt") == "ERROR":
                    raise RpcError(reply["data"].get("message", "unknown error"))
                return reply.get("data")
            self._handle_event(reply)

    def _wait_for_data(self):
        """Waits up to IDLE_S for Discord to send something. A plain read would
        block until it does, and Windows won't let us write to the pipe while
        a read is pending, so commands couldn't be sent meanwhile."""
        handle = msvcrt.get_osfhandle(self.pipe.fileno())
        _, available, _ = win32pipe.PeekNamedPipe(handle, 0)
        if not available:
            time.sleep(IDLE_S)
        return available > 0

    # ---------- commands ----------

    # Queued here and sent by the session thread, which owns the pipe.

    def leave(self):
        """Leaves the current voice channel."""
        self.commands.put(("SELECT_VOICE_CHANNEL", {"channel_id": None, "force": True}, "left voice channel"))

    def set_mute(self, muted):
        # Unmuting also undeafens, like clicking the mic in Discord.
        args = {"mute": True} if muted else {"mute": False, "deaf": False}
        self.commands.put(("SET_VOICE_SETTINGS", args, "muted" if muted else "unmuted"))

    def set_deaf(self, deaf):
        self.commands.put(("SET_VOICE_SETTINGS", {"deaf": bool(deaf)}, "deafened" if deaf else "undeafened"))

    def _run_commands(self):
        while not self.commands.empty():
            cmd, args, done = self.commands.get()
            try:
                self._call(cmd, args)
                log.info(done)
            except RpcError as e:
                log.warning("%s failed: %s", cmd, e)

    def _handle_event(self, msg):
        evt, data = msg.get("evt"), msg.get("data") or {}
        if evt == "VOICE_CHANNEL_SELECT":
            self._follow_channel(data.get("channel_id"))
        elif evt in ("VOICE_STATE_CREATE", "VOICE_STATE_UPDATE"):
            self._upsert_user(data)
        elif evt == "VOICE_STATE_DELETE":
            uid = data.get("user", {}).get("id")
            with self.lock:
                if self.channel:
                    self.channel["users"].pop(uid, None)
                    self.version += 1
        elif evt == "SPEAKING_START":
            self._set_speaking(data.get("user_id"), True)
        elif evt == "SPEAKING_STOP":
            self._set_speaking(data.get("user_id"), False)
        elif evt == "VOICE_SETTINGS_UPDATE":
            self._set_self(data)
        elif evt == "NOTIFICATION_CREATE":
            self.on_notify(data.get("title") or "Discord", data.get("body") or "")

    # ---------- voice channel ----------

    def _follow_channel(self, channel_id):
        if self.channel_id:
            for evt in VOICE_EVENTS:
                try:
                    self._call("UNSUBSCRIBE", {"channel_id": self.channel_id}, evt)
                except RpcError:
                    pass
        self.channel_id = None
        if not channel_id:
            self._set(channel=None)
            return

        ch = self._call("GET_CHANNEL", {"channel_id": channel_id})
        guild = "Direct call"
        if ch.get("guild_id"):
            try:
                guild = self._call("GET_GUILD", {"guild_id": ch["guild_id"]}).get("name", "")
            except RpcError:
                guild = ""
        self._set(channel={"name": ch.get("name") or "Voice", "guild": guild, "users": {}})
        for state in ch.get("voice_states", []):
            self._upsert_user(state)
        for evt in VOICE_EVENTS:
            self._call("SUBSCRIBE", {"channel_id": channel_id}, evt)
        self.channel_id = channel_id

    # ---------- auth ----------

    def _token_request(self, cfg, fields):
        body = urllib.parse.urlencode({"client_id": cfg["client_id"], "client_secret": cfg["client_secret"], **fields})
        req = urllib.request.Request(
            "https://discord.com/api/oauth2/token", data=body.encode(),
            headers={"Content-Type": "application/x-www-form-urlencoded", "User-Agent": "KnobAgent/1.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            tok = json.load(resp)
        tok["expires_at"] = time.time() + tok.get("expires_in", 604800) - 3600
        TOKEN_FILE.write_text(json.dumps(tok))
        return tok

    def _access_token(self, cfg):
        tok = json.loads(TOKEN_FILE.read_text()) if TOKEN_FILE.exists() else None
        # A token from before a scope was added has to be approved again.
        if tok and not set(SCOPES) <= set(tok.get("scope", "").split()):
            log.info("token is missing scopes, asking Discord again")
            tok = None
        if tok and time.time() < tok.get("expires_at", 0):
            return tok["access_token"]
        if tok and tok.get("refresh_token"):
            try:
                return self._token_request(cfg, {"grant_type": "refresh_token",
                                                 "refresh_token": tok["refresh_token"]})["access_token"]
            except Exception as e:
                log.warning("token refresh failed: %s", e)
        # First run: Discord shows an approval prompt in the app.
        self._set(status="approve")
        # AUTHORIZE rejects a redirect_uri, but Discord still requires one to be
        # registered on the app, and the token exchange below must send it.
        code = self._call("AUTHORIZE", {"client_id": cfg["client_id"], "scopes": SCOPES})["code"]
        return self._token_request(cfg, {"grant_type": "authorization_code", "code": code,
                                         "redirect_uri": REDIRECT_URI})["access_token"]

    # ---------- main loop ----------

    def _connect(self):
        for i in range(10):
            try:
                return open(rf"\\.\pipe\discord-ipc-{i}", "r+b", buffering=0)
            except OSError:
                pass
        return None

    def _session(self):
        try:
            cfg = json.loads(CONFIG_FILE.read_text())
        except (OSError, ValueError):
            cfg = {}
        if not cfg.get("client_id") or not cfg.get("client_secret"):
            self._set(status="no_config", channel=None)
            time.sleep(30)
            return

        self.pipe = self._connect()
        if not self.pipe:
            self._set(status="no_discord", channel=None)
            time.sleep(5)
            return
        try:
            self._send(OP_HANDSHAKE, {"v": 1, "client_id": cfg["client_id"]})
            self._recv()   # READY
            try:
                self._call("AUTHENTICATE", {"access_token": self._access_token(cfg)})
            except RpcError as e:
                if "denied" in str(e).lower() or "cancel" in str(e).lower():
                    self._set(status="denied")
                    time.sleep(60)
                    return
                log.warning("authenticate failed (%s), clearing token", e)
                TOKEN_FILE.unlink(missing_ok=True)
                raise
            self._set(status="ok")
            self._call("SUBSCRIBE", {}, "VOICE_CHANNEL_SELECT")
            self._call("SUBSCRIBE", {}, "VOICE_SETTINGS_UPDATE")
            self._set_self(self._call("GET_VOICE_SETTINGS") or {})
            try:
                self._call("SUBSCRIBE", {}, "NOTIFICATION_CREATE")
            except RpcError as e:
                log.warning("notifications unavailable: %s", e)
            selected = self._call("GET_SELECTED_VOICE_CHANNEL")
            self._follow_channel(selected.get("id") if selected else None)
            # Taps from while Discord was unreachable are stale by now.
            while not self.commands.empty():
                self.commands.get()
            while True:
                self._run_commands()
                if self._wait_for_data():
                    self._handle_event(self._recv())
        finally:
            self.pipe.close()
            self.pipe = None
            self.channel_id = None

    def _run(self):
        while True:
            try:
                self._session()
            except Exception as e:
                log.warning("discord session ended: %s", e)
                if self.status in ("ok", "approve", "starting"):
                    self._set(status="error", channel=None)
                time.sleep(5)
