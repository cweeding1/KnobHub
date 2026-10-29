# KnobHub

A desk dashboard for the **Waveshare ESP32-S3 Knob Touch LCD 1.8**: a round
touchscreen inside a clicky rotary dial. KnobHub turns it into a small hub for
your PC:

- **Music**: Spotify now playing with full-screen album art. Turn the dial for volume, and tap the heart to like a song.
- **PC Stats**: CPU and GPU load and temperatures, RAM, VRAM and network speed.
- **Mixer**: Windows volume for the whole PC or for each app.
- **Discord**: who's in your voice channel and who's talking. Mute, deafen and leave from the dial.
- **Notifications**: Discord messages pop up over any screen.
- **Settings**: brightness, auto-dim, screen-off timers and haptic strength.

Music connects straight to Spotify over Wi-Fi. Everything else comes from a
small Python agent on a Windows PC, over the same USB cable that powers the knob.

## Contents

- [Hardware](#hardware)
- [How it works](#how-it-works)
- [Using it](#using-it)
- [Setup](#setup)
  - [1. Flash the firmware](#1-flash-the-firmware)
  - [2. Run the PC agent](#2-run-the-pc-agent)
  - [3. Spotify (optional)](#3-spotify-optional)
  - [4. Discord (optional)](#4-discord-optional)
  - [5. Temperatures (optional)](#5-temperatures-optional)
- [Troubleshooting](#troubleshooting)
- [Project layout](#project-layout)
- [Serial protocol](#serial-protocol)
- [Credits and license](#credits-and-license)

## Hardware

| | |
|---|---|
| Device | [Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/esp32-s3-knob-touch-lcd-1.8.htm) |
| Chip | ESP32-S3, 16 MB flash, 8 MB octal PSRAM |
| Display | 1.8" round 360×360 LCD (SH8601, QSPI) |
| Touch | CST816 |
| Haptics | DRV2605 motor driver |
| Input | Rotary dial with detents |
| Cable | USB-C data cable to your PC |

Nothing else is needed: no soldering and no extra parts.

> **About the USB-C plug.** The board has two USB chips. With the plug one way
> round, Windows sees the ESP32-S3 itself (`USB VID 303A, PID 1001`, shown as a
> plain "USB Serial Device"). Flipped over, it sees a **CH340** serial adapter
> instead. KnobHub needs the ESP32-S3 port, for both flashing and the PC
> agent, so if you see a CH340 in Device Manager, flip the plug.

Pins used, in case you're adapting it to another board:

| Function | GPIO |
|---|---|
| LCD QSPI | CS 14, CLK 13, D0–D3 15–18, RST 21 |
| Backlight (PWM) | 47 |
| Dial | A 8, B 7 |
| I²C (touch at 0x15, DRV2605) | SDA 11, SCL 12 |

## How it works

```
                 Wi-Fi                          USB serial (JSON lines)
  Spotify Web API <────> Knob (ESP32-S3) <────────────────> pc_agent.py (Windows)
                                                             ├─ CPU/GPU/RAM/network (psutil, PDH)
                                                             ├─ temperatures (LibreHardwareMonitor)
                                                             ├─ volume mixer (Windows Core Audio)
                                                             ├─ lock screen (WTS session events)
                                                             └─ Discord (local RPC pipe)
```

- **Firmware** is an Arduino sketch: `KnobHub.ino` plus one `screen_*.cpp`
  file per app. It draws each frame into a PSRAM framebuffer and pushes it to
  the panel over QSPI DMA.
- **The PC agent** finds the knob by its USB ID, about 10 times a second. It
  sends stats, mixer and Discord state, and takes volume, mute and Discord
  commands back. It reconnects by itself when the knob is unplugged or reflashed.

Without the agent, Music and Settings still work, and the PC apps show
"Plug into your PC".

## Using it

**Everywhere**
- The **band across the top** of the screen is the home button. A line under
  the house icon marks where it ends.
- When the screen has dimmed or gone off, the first touch or dial turn only
  wakes it. It doesn't also press anything.
- Discord notifications pop up for 6 seconds. Tap to dismiss.

| App | Dial | Tap | Swipe |
|---|---|---|---|
| **Home** | Pick an app | Middle: open it. Sides: neighbouring app | |
| **Music** | Volume (ring shows briefly) | Middle: play/pause. Right: next. Left: restart song, or previous track near the start. Bottom band (heart): add to / remove from Liked Songs | Left: next. Right: same as tapping left |
| **PC Stats** | | | |
| **Mixer** | Volume of the shown app (ring shows briefly) | Middle: mute. Sides: switch app | Switch app |
| **Discord** | Choose **Mute**, **Leave call** or **Deafen** in the bottom bar | The bottom bar does what it shows. Leaving needs a second tap to confirm | Up/down scrolls the member list |
| **Settings** | Change the value (ring shows briefly) | Middle: flip on/off settings. Sides: next/previous setting | Next/previous setting |

**Settings** (saved on the knob)

| Setting | Options | Default |
|---|---|---|
| Brightness | 5–100% | 100% |
| Dim after | Never, 15 s – 10 min | 1 min |
| Dim to | 5–50% | 10% |
| Screen off after | Never, 1 min – 1 hr | 10 min |
| Off when PC locks | On / Off | On |
| Notifications wake | On / Off | On |
| Haptics | Off / Light / Strong | Strong |

"Off when PC locks" turns the screen off as soon as Windows locks (Win+L or a
timeout). It comes back on when you unlock or touch the knob.

## Setup

### 1. Flash the firmware

You can use the Arduino IDE (2.x) or `arduino-cli`.

**Board support**
- Install **esp32 by Espressif Systems**, version **3.3.x** (tested with 3.3.12).
- In the IDE: *Boards Manager*, search "esp32".
- With the CLI: `arduino-cli core install esp32:esp32`.

**Libraries** (Library Manager / `arduino-cli lib install "<name>"`)

| Library | Tested version |
|---|---|
| Adafruit GFX Library (and its dependency Adafruit BusIO) | 1.12.5 |
| ArduinoJson | 7.4.3 |
| JPEGDEC | 1.8.4 |
| SensorLib (by Lewis He) | 0.4.0 |

The display, dial and touch drivers are already included in this folder.

**Get the code.** Clone or download the repo. The folder must be named
`KnobHub` to match `KnobHub.ino`, because Arduino requires it.

**Board settings**

| IDE menu | Value | CLI option |
|---|---|---|
| Board | ESP32S3 Dev Module | `esp32:esp32:esp32s3` |
| USB CDC On Boot | Enabled | `CDCOnBoot=cdc` |
| Flash Size | 16MB (128Mb) | `FlashSize=16M` |
| PSRAM | OPI PSRAM | `PSRAM=opi` |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) | `PartitionScheme=app3M_fat9M_16MB` |

**Build and upload** to the ESP32-S3 port (see the USB note above):

```
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB KnobHub
arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB KnobHub
```

Replace `COM3` with your port. The firmware builds without any credentials;
Music will just say "Setup needed" until you do step 3.

> If the PC agent is running, **stop it before uploading**. It holds the COM
> port open.

### 2. Run the PC agent

The agent is Windows-only, because it uses Windows audio, lock-screen and
performance APIs. It's tested with Python 3.13.

```
cd KnobHub
py -m pip install --user -r requirements.txt
py pc_agent.py
```

The agent logs to `pc_agent.log` next to the script. Look for
`connected to knob on COMx` there.

**Run it in the background.** `pythonw.exe` runs it without a console window:

```
pythonw pc_agent.py
```

Start it from inside the `KnobHub` folder, or give the full path to
`pc_agent.py`. It imports `discord_rpc.py` from the same folder. Don't add
Python's `-P` flag, which stops that import working.

**Start it with Windows.** Press Win+R, run `shell:startup`, and create a
shortcut in that folder:

- **Target:** `"C:\path\to\pythonw.exe" "C:\path\to\KnobHub\pc_agent.py"`
- **Start in:** `C:\path\to\KnobHub`

To find `pythonw.exe`, run
`py -c "import sys, os; print(os.path.join(os.path.dirname(sys.executable), 'pythonw.exe'))"`.

**Stop it**, for example before flashing. Either end the `pythonw.exe` process
in Task Manager, or run this in PowerShell:

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*pc_agent.py*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

### 3. Spotify (optional)

You need **Spotify Premium**. Playback control needs it, and since February
2026 Spotify also requires a developer app's owner to have Premium. A
development-mode app is limited to 5 users, which is plenty for your own knob.

1. Go to the [Spotify developer dashboard](https://developer.spotify.com/dashboard)
   and **create an app**:
   - Redirect URI: exactly `http://127.0.0.1:8888/callback`
   - APIs used: **Web API**
2. Copy `secrets.example.h` to `secrets.h` and fill in:
   - your **Wi-Fi** name and password (2.4 GHz only, as the ESP32 can't use 5 GHz)
   - the app's **Client ID** and **Client Secret**
3. Run the one-time login. A browser opens; log in and approve.

   ```
   py spotify_auth.py
   ```

   This writes `SPOTIFY_REFRESH_TOKEN` into `secrets.h`.
4. Build and upload the firmware again.

Start playing something on any Spotify device and it shows up on the knob.
The heart turns green when the song is in your Liked Songs. It also picks up
likes you make in the Spotify app, within about 15 seconds.
When Spotify issues a new refresh token, the knob saves it in flash, so you
only need to log in once.

`secrets.h` is in `.gitignore`. Don't commit it.

### 4. Discord (optional)

The Discord app and the notification pop-ups talk to the **Discord desktop
app** on the same PC, through its local RPC interface. Discord only lets an
application's owner (and its listed testers) use RPC. So everyone makes their
own Discord application; it takes a minute:

1. Go to the [Discord developer portal](https://discord.com/developers/applications)
   and click **New Application**. Name it anything, for example "KnobHub".
2. Under **OAuth2**:
   - Copy the **Client ID**.
   - Click **Reset Secret** and copy the **Client Secret**.
   - Add the redirect `http://127.0.0.1`. Discord requires one to be
     registered, though nothing is served there.
3. Copy `discord.example.json` to `discord.json` and paste in both values.
4. Start (or restart) the agent with Discord running. Discord shows an
   **authorize** pop-up; accept it.

The agent saves the resulting token in `discord_token.json`, which is
gitignored, and refreshes it automatically. If a future version needs more
permissions, the agent asks you to approve again.

Permissions requested: `rpc`, `rpc.voice.read`, `rpc.voice.write` (for mute,
deafen and leave) and `rpc.notifications.read`.

### 5. Temperatures (optional)

CPU and GPU temperatures come from
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor):

1. Run LibreHardwareMonitor as administrator.
2. Turn on **Options → Remote Web Server → Run**, on the default port **8085**.

Without it, PC Stats shows load and memory but no temperatures.

## Troubleshooting

| Problem | Fix |
|---|---|
| Upload fails or the port is busy | Stop the PC agent first. Make sure you're on the ESP32-S3 port, not the CH340: flip the USB-C plug. |
| PC apps say "Plug into your PC" | Check the agent is running and `pc_agent.log` shows `connected to knob`. Only one program can hold the port at a time, so close any serial monitor. |
| Screen is black after flashing | Turn the dial or touch it, as it may just be asleep. Check *Settings → Brightness*. |
| Music: "Setup needed" | `secrets.h` is missing or incomplete, or you didn't re-flash after `spotify_auth.py`. |
| Music: "Spotify login failed" | Run `spotify_auth.py` again and re-flash. |
| Music: "Nothing playing" | Start playback on any Spotify device. Ads and private sessions also show this. |
| Music: no heart | Your login predates the Liked Songs permission. Run `spotify_auth.py` again and re-flash. Local files can't be liked, so they never show it. |
| Discord: "Not set up" | Fill in `discord.json`. |
| Discord: "Check Discord" | Accept the authorize pop-up in the Discord app. |
| Discord: "Access denied" | You declined the pop-up. Restart the agent to be asked again. |
| Discord: no mute/deafen effect | Check `pc_agent.log` for `SET_VOICE_SETTINGS failed`. Delete `discord_token.json` and restart the agent to re-authorize. |
| Dial turns the wrong way | Set `SWAP_KNOB_DIRECTION` to `1` in `KnobHub.ino`. |
| No temperatures | Start LibreHardwareMonitor with its web server on port 8085 (step 5). |

## Project layout

| File | What it does |
|---|---|
| `KnobHub.ino` | Main loop: input (dial, touch, haptics), app switching, auto-dim, redraw logic |
| `screen_home.cpp` | Home carousel and app icons |
| `screen_music.cpp`, `spotify.cpp/.h` | Music screen; Spotify Web API client on its own task, JPEG album art |
| `screen_pc.cpp` | PC Stats gauges and bars |
| `screen_mixer.cpp` | Volume mixer |
| `screen_voice.cpp` | Discord voice channel and the mute / leave / deafen bar |
| `screen_notification.cpp` | Notification pop-up |
| `screen_settings.cpp`, `settings.cpp/.h` | Settings screen and saved settings |
| `pc_link.cpp/.h` | Serial link to the PC agent: parses messages, sends commands |
| `gfx.cpp/.h`, `text.cpp/.h` | Framebuffer, drawing helpers, text fitting |
| `display.c/.h` | Panel and backlight driver |
| `esp_lcd_sh8601.*`, `lcd_init_cmds.h`, `bidi_switch_knob.*` | Display and dial drivers from Espressif / Waveshare |
| `secrets.example.h` | Template for `secrets.h` (Wi-Fi and Spotify) |
| `pc_agent.py` | The PC agent |
| `discord_rpc.py` | Minimal Discord local-RPC client used by the agent |
| `spotify_auth.py` | One-time Spotify login that fills in the refresh token |
| `discord.example.json` | Template for `discord.json` |
| `requirements.txt` | Python packages for the agent |

## Serial protocol

The link runs at 115200 baud over the ESP32-S3's USB-CDC port. Each message is
one JSON object per line.

**PC to knob.** Each message's `t` field gives its type:

| `t` | Sent | Fields |
|---|---|---|
| `stats` | every second | `host`, `cpu`, `gpu` (%), `ram`/`vram` (`[used, total]` GB), `down`/`up` (bytes/s), `ct`/`gt` (°C or null), `lk` (1 = locked) |
| `mixer` | on change, and every 3 s | `s`: `[{id, n, v, m}]` (id, name, volume 0–100, muted) |
| `voice` | on change, and every 3 s | `st` (status), `ch`, `g` (channel, server), `sm`/`sd` (your mute/deafen), `u`: `[{id, n, s, m, d}]` |
| `avatar` | once per member | `id`, `px` (base64 32×32 RGB565) |
| `notify` | as they arrive | `title`, `body` |

**Knob to PC.** Each command's `cmd` field gives its type:

| `cmd` | Fields |
|---|---|
| `vol` | `id`, `v` (0–100) |
| `mute` | `id`, `m` (0/1) |
| `leave` | (none) |
| `dmute` | `m` (0/1): your Discord mute |
| `ddeaf` | `d` (0/1): your Discord deafen |

## Credits and license

- `esp_lcd_sh8601.*` and `bidi_switch_knob.*` are Espressif components
  (Apache-2.0, see their headers). The dial driver has changes from Waveshare's
  demo code for this board.
- `lcd_init_cmds.h` is the panel init sequence from Waveshare's demo code.
- Everything else is under the [MIT License](LICENSE).

This project isn't affiliated with Spotify, Discord or Waveshare.
