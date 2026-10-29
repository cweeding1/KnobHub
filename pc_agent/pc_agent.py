"""Links the knob to this PC over USB serial.

Sends one JSON line per message to the knob:
  stats   CPU/GPU load, RAM/VRAM, network, temperatures,
          whether the PC is locked                        (every second)
  mixer   master + per-app volume                          (on change)
  voice   current Discord voice channel and its members    (on change)
  notify  a Discord notification                           (as they arrive)
  avatar  a Discord member's picture                       (once per member)
and receives volume/mute/leave and Discord mute/deafen commands back from the knob.

Run it with pythonw.exe for no console window; it logs to pc_agent.log.
Finds the knob by its USB ID and reconnects whenever it is plugged back in.

Needs: pip install --user -r requirements.txt
Temperatures need LibreHardwareMonitor running with its web server on port 8085.

Stop this before flashing new firmware: it holds the COM port open.
"""
import base64
import collections
import ctypes
import io
import json
import logging
import pathlib
import queue
import re
import socket
import struct
import threading
import time
import urllib.request
import winreg

import psutil
import serial
import win32api
import win32gui
import win32pdh
import win32ts
from PIL import Image
from pycaw.pycaw import AudioUtilities
from serial.tools import list_ports

from discord_rpc import DiscordVoice

KNOB_VID, KNOB_PID = 0x303A, 0x1001   # Espressif USB-Serial/JTAG
GPU_CLASS_KEY = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
LUID_RE = re.compile(r"luid_(0x[0-9A-Fa-f]+_0x[0-9A-Fa-f]+)")
LHM_URL = "http://localhost:8085/data.json"
AVATAR_SIZE = 32   # must match AVATAR_SIZE in firmware/KnobHub/src/services/pc_link.h
GB = 1024 ** 3
TICK = 0.1
RESEND_S = 3   # resend mixer/voice this often even without changes

HERE = pathlib.Path(__file__).resolve().parent
logging.basicConfig(filename=HERE / "pc_agent.log", level=logging.INFO,
                    format="%(asctime)s %(name)s %(levelname)s %(message)s")
log = logging.getLogger("agent")


# ---------- GPU load ----------

def total_vram_bytes():
    """Largest dedicated VRAM size among the installed display adapters."""
    best = 0
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, GPU_CLASS_KEY) as cls:
        for i in range(winreg.QueryInfoKey(cls)[0]):
            try:
                with winreg.OpenKey(cls, winreg.EnumKey(cls, i)) as dev:
                    best = max(best, winreg.QueryValueEx(dev, "HardwareInformation.qwMemorySize")[0])
            except OSError:
                pass
    return best


class GpuStats:
    """GPU load the way Task Manager computes it: per adapter, the busiest engine
    type, summed over processes. Reports the adapter using the most dedicated
    memory, which is the discrete card rather than the integrated one."""

    def __init__(self):
        self.query = win32pdh.OpenQuery()
        self.engines = win32pdh.AddEnglishCounter(self.query, r"\GPU Engine(*)\Utilization Percentage")
        self.memory = win32pdh.AddEnglishCounter(self.query, r"\GPU Adapter Memory(*)\Dedicated Usage")
        self.vram_total = total_vram_bytes()
        win32pdh.CollectQueryData(self.query)

    def sample(self):
        win32pdh.CollectQueryData(self.query)
        try:
            engines = win32pdh.GetFormattedCounterArray(self.engines, win32pdh.PDH_FMT_DOUBLE)
            memory = win32pdh.GetFormattedCounterArray(self.memory, win32pdh.PDH_FMT_LARGE)
        except win32pdh.error:
            return 0.0, 0
        luid_memory = {}
        for name, used in memory.items():
            m = LUID_RE.search(name)
            if m:
                luid_memory[m.group(1)] = used
        if not luid_memory:
            return 0.0, 0
        luid = max(luid_memory, key=luid_memory.get)

        per_engine = collections.defaultdict(float)
        for name, value in engines.items():
            if luid in name:
                per_engine[name.rsplit("engtype_", 1)[-1]] += value
        load = min(100.0, max(per_engine.values(), default=0.0))
        return load, luid_memory[luid]


# ---------- Temperatures ----------

def read_temperatures():
    """CPU package and GPU core temperatures from LibreHardwareMonitor, or None."""
    try:
        with urllib.request.urlopen(LHM_URL, timeout=0.3) as resp:
            root = json.load(resp)
    except Exception:
        return None, None

    cpu = gpu = None
    stack = [root]
    while stack:
        node = stack.pop()
        stack.extend(node.get("Children", []))
        sensor = node.get("SensorId", "")
        if "/temperature/" not in sensor:
            continue
        m = re.match(r"\s*(-?[\d.,]+)", node.get("Value", ""))
        if not m:
            continue
        value = float(m.group(1).replace(",", "."))
        text = node.get("Text", "")
        # Prefer the headline sensor; otherwise take the first one seen.
        if sensor.startswith(("/intelcpu/", "/amdcpu/")):
            if text in ("CPU Package", "Core (Tctl/Tdie)") or cpu is None:
                cpu = value
        elif sensor.startswith("/gpu"):
            if text == "GPU Core" or gpu is None:
                gpu = value
    return cpu, gpu


# ---------- Lock screen ----------

DESKTOP_SWITCHDESKTOP = 0x0100
WM_WTSSESSION_CHANGE = 0x02B1
WTS_SESSION_LOCK, WTS_SESSION_UNLOCK = 7, 8
user32 = ctypes.windll.user32


def password_prompt_up():
    """True while the password prompt is up: the input desktop can't be switched to then."""
    desk = user32.OpenInputDesktop(0, False, DESKTOP_SWITCHDESKTOP)
    if not desk:
        return True
    try:
        return not user32.SwitchDesktop(desk)
    finally:
        user32.CloseDesktop(desk)


class LockWatcher:
    """Follows Windows' lock / unlock events from a hidden window. The events
    fire as soon as the clock screen appears, before the password prompt."""

    def __init__(self):
        self.locked = False   # the agent starts at login, so unlocked
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        wc = win32gui.WNDCLASS()
        wc.lpszClassName = "KnobAgentLockWatcher"
        wc.hInstance = win32api.GetModuleHandle(None)
        wc.lpfnWndProc = {WM_WTSSESSION_CHANGE: self._on_session_change}
        win32gui.RegisterClass(wc)
        hwnd = win32gui.CreateWindow(wc.lpszClassName, "", 0, 0, 0, 0, 0, 0, 0, wc.hInstance, None)
        win32ts.WTSRegisterSessionNotification(hwnd, win32ts.NOTIFY_FOR_THIS_SESSION)
        win32gui.PumpMessages()

    def _on_session_change(self, hwnd, msg, wparam, lparam):
        if wparam in (WTS_SESSION_LOCK, WTS_SESSION_UNLOCK):
            self.locked = wparam == WTS_SESSION_LOCK
            log.info("PC %s", "locked" if self.locked else "unlocked")
        return 0

    def is_locked(self):
        return self.locked or password_prompt_up()


# ---------- Volume mixer ----------

class Mixer:
    def __init__(self):
        self.names = {}

    def _friendly_name(self, exe, process):
        if exe not in self.names:
            name = pathlib.Path(exe).stem.title()
            try:
                path = process.exe()
                lang, codepage = win32api.GetFileVersionInfo(path, "\\VarFileInfo\\Translation")[0]
                desc = win32api.GetFileVersionInfo(
                    path, f"\\StringFileInfo\\{lang:04X}{codepage:04X}\\FileDescription")
                if desc and len(desc) <= 24:
                    name = desc
            except Exception:
                pass
            self.names[exe] = name
        return self.names[exe]

    def _groups(self):
        groups = collections.defaultdict(list)
        for s in AudioUtilities.GetAllSessions():
            if s.Process:
                groups[s.Process.name().lower()].append(s)
        return groups

    def snapshot(self):
        master = AudioUtilities.GetSpeakers().EndpointVolume
        sessions = [{"id": "master", "n": "Master",
                     "v": round(master.GetMasterVolumeLevelScalar() * 100), "m": int(master.GetMute())}]
        apps = []
        for exe, group in self._groups().items():
            vol = group[0].SimpleAudioVolume
            apps.append({"id": exe, "n": self._friendly_name(exe, group[0].Process),
                         "v": round(vol.GetMasterVolume() * 100), "m": int(vol.GetMute())})
        return sessions + sorted(apps, key=lambda a: a["n"].lower())

    def set_volume(self, sid, percent):
        level = max(0, min(100, percent)) / 100
        if sid == "master":
            AudioUtilities.GetSpeakers().EndpointVolume.SetMasterVolumeLevelScalar(level, None)
        else:
            for s in self._groups().get(sid, []):
                s.SimpleAudioVolume.SetMasterVolume(level, None)

    def set_mute(self, sid, muted):
        if sid == "master":
            AudioUtilities.GetSpeakers().EndpointVolume.SetMute(int(muted), None)
        else:
            for s in self._groups().get(sid, []):
                s.SimpleAudioVolume.SetMute(int(muted), None)


# ---------- Discord avatars ----------

class Avatars:
    """Downloads Discord avatars in the background, shrunk to AVATAR_SIZE and
    packed as base64 little-endian RGB565, ready to send to the knob."""

    def __init__(self):
        self.cache = {}            # (user_id, avatar_hash) -> base64 str, or None if it failed
        self.wanted = queue.Queue()
        self.lock = threading.Lock()
        threading.Thread(target=self._run, daemon=True).start()

    def get(self, user_id, avatar_hash):
        key = (user_id, avatar_hash)
        with self.lock:
            if key in self.cache:
                return self.cache[key]
            self.cache[key] = None   # mark as requested
        self.wanted.put(key)
        return None

    @staticmethod
    def _url(user_id, avatar_hash):
        if avatar_hash:
            return f"https://cdn.discordapp.com/avatars/{user_id}/{avatar_hash}.png?size=64"
        return f"https://cdn.discordapp.com/embed/avatars/{(int(user_id) >> 22) % 6}.png"

    def _run(self):
        while True:
            user_id, avatar_hash = key = self.wanted.get()
            try:
                req = urllib.request.Request(self._url(user_id, avatar_hash), headers={"User-Agent": "KnobAgent/1.0"})
                with urllib.request.urlopen(req, timeout=10) as resp:
                    img = Image.open(io.BytesIO(resp.read())).convert("RGB")
                img = img.resize((AVATAR_SIZE, AVATAR_SIZE), Image.LANCZOS)
                px = b"".join(struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
                              for r, g, b in img.getdata())
                with self.lock:
                    self.cache[key] = base64.b64encode(px).decode()
            except Exception as e:
                log.warning("avatar %s failed: %s", user_id, e)


# ---------- Serial link ----------

def find_knob_port():
    for port in list_ports.comports():
        if port.vid == KNOB_VID and port.pid == KNOB_PID:
            return port.device
    return None


def open_port(device):
    s = serial.Serial()
    s.port = device
    s.baudrate = 115200
    s.timeout = 0
    s.write_timeout = 1
    # Leave DTR and RTS low: toggling them is how esptool resets the chip.
    s.dtr = False
    s.rts = False
    s.open()
    return s


class Link:
    """The serial connection to the knob, reopened whenever it drops."""

    def __init__(self):
        self.port = None
        self.buf = b""
        self.last_try = 0
        self.sent_avatars = set()   # what the knob has, reset when it reconnects

    def connected(self):
        if self.port is None and time.monotonic() - self.last_try > 1:
            self.last_try = time.monotonic()
            device = find_knob_port()
            if device:
                try:
                    self.port = open_port(device)
                    log.info("connected to knob on %s", device)
                except serial.SerialException:
                    pass   # busy, e.g. while firmware is being flashed
        return self.port is not None

    def _drop(self):
        log.info("knob disconnected")
        self.port.close()
        self.port = None
        self.buf = b""
        self.sent_avatars.clear()

    def send(self, msg):
        if not self.connected():
            return False
        try:
            self.port.write((json.dumps(msg, separators=(",", ":")) + "\n").encode())
            return True
        except (serial.SerialException, OSError):
            self._drop()
            return False

    def commands(self):
        """Commands the knob sent; other output (debug logs) is ignored."""
        if not self.connected():
            return []
        try:
            self.buf += self.port.read(self.port.in_waiting or 1)
        except (serial.SerialException, OSError):
            self._drop()
            return []
        *lines, self.buf = self.buf.split(b"\n")
        out = []
        for line in lines:
            line = line.strip()
            if line.startswith(b'{"cmd"'):
                try:
                    out.append(json.loads(line))
                except ValueError:
                    pass
        return out


# ---------- Main loop ----------

def main():
    gpu = GpuStats()
    mixer = Mixer()
    notifications = queue.Queue()
    discord = DiscordVoice(lambda title, body: notifications.put((title, body)))
    link = Link()
    avatars = Avatars()
    lock = LockWatcher()

    host = socket.gethostname()
    psutil.cpu_percent()
    last_net, last_stats = psutil.net_io_counters(), time.monotonic()
    last_mixer = last_voice = None
    mixer_sent = voice_sent = 0

    while True:
        time.sleep(TICK)
        now = time.monotonic()
        mixer_dirty = False

        for cmd in link.commands():
            try:
                if cmd["cmd"] == "vol":
                    mixer.set_volume(cmd["id"], cmd["v"])
                elif cmd["cmd"] == "mute":
                    mixer.set_mute(cmd["id"], cmd["m"])
                elif cmd["cmd"] == "leave":
                    discord.leave()
                elif cmd["cmd"] == "dmute":
                    discord.set_mute(cmd["m"])
                elif cmd["cmd"] == "ddeaf":
                    discord.set_deaf(cmd["d"])
                mixer_dirty = True
            except Exception as e:
                log.warning("command %s failed: %s", cmd, e)

        if now - last_stats >= 1:
            net = psutil.net_io_counters()
            elapsed = now - last_stats
            ram = psutil.virtual_memory()
            load, vram_used = gpu.sample()
            cpu_temp, gpu_temp = read_temperatures()
            link.send({
                "t": "stats", "host": host,
                "cpu": round(psutil.cpu_percent(), 1), "gpu": round(load, 1),
                "ram": [round(ram.used / GB, 1), round(ram.total / GB, 1)],
                "vram": [round(vram_used / GB, 1), round(gpu.vram_total / GB, 1)],
                "down": round((net.bytes_recv - last_net.bytes_recv) / elapsed),
                "up": round((net.bytes_sent - last_net.bytes_sent) / elapsed),
                "ct": cpu_temp, "gt": gpu_temp, "lk": int(lock.is_locked()),
            })
            last_net, last_stats = net, now
            mixer_dirty = True   # also re-read the mixer once a second

        if mixer_dirty:
            try:
                sessions = mixer.snapshot()
            except Exception as e:
                log.warning("mixer read failed: %s", e)
                sessions = last_mixer
            if sessions is not None and (sessions != last_mixer or now - mixer_sent >= RESEND_S):
                if link.send({"t": "mixer", "s": sessions}):
                    last_mixer, mixer_sent = sessions, now

        status, channel, (self_mute, self_deaf), version = discord.snapshot()
        if version != last_voice or now - voice_sent >= RESEND_S:
            msg = {"t": "voice", "st": status, "sm": int(self_mute), "sd": int(self_deaf)}
            if channel:
                msg.update(ch=channel["name"], g=channel["guild"],
                           u=[{"id": u["id"], "n": u["name"], "s": int(u["speaking"]),
                               "m": int(u["muted"]), "d": int(u["deaf"])}
                              for u in channel["users"][:16]])
            if link.send(msg):
                last_voice, voice_sent = version, now

        # Send each member's avatar once it has downloaded, one per tick to keep the link responsive.
        for u in channel["users"][:16] if channel else []:
            key = (u["id"], u["avatar"])
            if key in link.sent_avatars:
                continue
            px = avatars.get(*key)
            if px and link.send({"t": "avatar", "id": u["id"], "px": px}):
                link.sent_avatars.add(key)
                log.info("sent avatar for %s", u["name"])
                break

        while not notifications.empty():
            title, body = notifications.get()
            link.send({"t": "notify", "title": title[:80], "body": body[:200]})


if __name__ == "__main__":
    try:
        main()
    except Exception:
        log.exception("agent crashed")
        raise
