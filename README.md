# Reflex

A multilingual reaction party-game for the ESP8266 (Wemos D1 Mini). Players each
hold a wired push-button; the device springs a sudden signal and the fastest
reaction wins. Two game modes, combo bonuses, persistent scores, and a retro web
scoreboard with a live leaderboard you open on a phone.

> The name **Reflex** stays the same in every language — only interface strings
> are translated (EN / DE / UK / RU).

## Variants

One codebase, two hardware builds. Each firmware is tagged with a `FW_VARIANT`
label so a device only ever pulls its own binary during OTA updates.

| Variant | Folder | Status | Scoreboard | Notes |
|---------|--------|--------|------------|-------|
| **Lite** (`lite`) | `firmware/reflex/` | active — **v0.5.0** | phone (web) | Wi-Fi menu, small I2C OLED for IP + winner, no service button |
| **TFT** (`tft`) | `firmware/reflex-tft/` | paused — v0.4.0 | on-board screen | native colour TFT; awaiting a replacement ILI9341 2.8" display |

The TFT build is paused: the original ST7789 module turned out to be dead, so the
on-screen variant resumes once an ILI9341 2.8" SPI display arrives.

## Game

- **Mode A — Who's faster:** after a random 1–10 s pause a sudden signal fires
  (buzzer); first valid press wins (+1) and the reaction time in ms is recorded.
  Pressing before the signal costs −1 but you can still win the round back.
- **Mode B — Most taps:** a sudden tap window of random length 2–10 s opens; most
  taps wins (+1), ties give every leader +1, early tappers are excluded.
- **Registration:** players hold their buttons together for 3 s (≥2) to start a
  2-round series, then release.
- **Combo:** every 3 consecutive wins grants +3. Streaks are per-mode.
- Scores and best reaction times are kept in RAM and mirrored to **LittleFS**, so
  they survive power-off.

## Lite hardware (current build)

- **Board:** Wemos D1 Mini (ESP8266)
- **Buttons:** 4 plain momentary switches to GND (one per player)
- **Buzzer:** passive piezo (square-wave chiptune via hardware Timer1)
- **Display:** 0.91" OLED (SSD1306 128×32, I2C) — shows the IP address / mode and
  congratulates the winner. The phone is the full scoreboard.
- No service button: all settings moved to the web UI.

### Pinout (Lite)

| Function | Wemos | GPIO | Note |
|----------|-------|------|------|
| Player P1 | D1 | 5 | `INPUT_PULLUP`, switch to GND |
| Player P2 | D2 | 4 | |
| Player P3 | D5 | 14 | |
| Player P4 | D6 | 12 | |
| Buzzer | D8 | 15 | passive piezo to GND, no pull-up |
| OLED SDA | D7 | 13 | software I2C |
| OLED SCL | D4 | 2 | software I2C |
| OLED VCC / GND | 3V3 / G | — | |

No button sits on a boot-strapping pin (GPIO0/D3, GPIO2/D4, GPIO15/D8), so a held
button never breaks boot. All buttons use internal pull-ups with software
debounce. Free for later: D3, D0.

### Wi-Fi flow

On first boot the device has no saved network, so it raises a config portal AP
named **Reflex-Setup** (WiFiManager). Connect a phone to it, pick your network
from the scan list, enter the password — it is remembered for next time. Once
joined, the assigned IP appears on the OLED; open it on a phone for the
scoreboard. If no known network is around, after a 3-minute timeout the device
falls back to its own AP **Reflex** and the game still works locally.

Settings (mode, sound, sound set, theme, language) are controlled from the web
page over WebSocket. Two maintenance commands were added: `{"set":"reset"}`
clears all scores, `{"set":"wifi","val":"reset"}` forgets the saved network so
you can reconfigure.

## Firmware

Arduino IDE, board *LOLIN(WeMos) D1 R2 & mini*. Open the variant you want
(`firmware/reflex/reflex.ino` for Lite); the `index_html_gz.h` tab sits next to
it and is included automatically.

Libraries (Library Manager, one click each) for **Lite**:
- **WiFiManager** by tzapu
- **U8g2** by oliver (kraus) — Cyrillic-capable OLED fonts
- **WebSockets** by Markus Sattler (Links2004 / arduinoWebSockets)
- `ESP8266WiFi`, `ESP8266WebServer`, `LittleFS` — bundled with the core

The web page is gzip-compressed into `index_html_gz.h` and served from flash;
state and settings travel over WebSocket (port 81).

## Web scoreboard

`web/index.html` is self-contained (no internet/CDN). Display-only — no on-screen
gameplay input. Open it directly for a demo playthrough; on hardware it goes LIVE
over WebSocket. Settings (stored in LittleFS on the device):

- **Languages:** English, Deutsch, Українська, Русский
- **Visual themes:** DOOM, Fallout, Grayscale, Amber, Neon
- **Sound sets:** DOOM, Arcade, Beep, Chip — plus master sound on/off
- **Game-mode switch** and a **leaderboard** (rank, score, best reaction in ms)

## OTA roadmap

OTA updates are staged on the Lite branch:

- `v0.5.1` — web upload (flash a `.bin` from the device's own page) + ArduinoOTA
  (upload from the IDE over Wi-Fi, no USB).
- `v0.5.2` — self-update from GitHub: a small `version.json` manifest with a
  per-variant section (version, binary URL, MD5) so each device fetches only its
  own release, plus a quiet "new version available — update" line in the web UI
  (no pop-ups).

## Roadmap

- `v0.1.0` — OLED firmware baseline
- `v0.2.0` — Wi-Fi AP + async web server + WebSocket
- `v0.3.0` — Wemos D1 Mini refactor: Timer1 square-wave audio, WebSocketsServer,
  LittleFS persistence, reaction-time leaderboard, web mode switch (OLED dropped)
- `v0.4.0` — TFT variant: native ST7789 240×320 scoreboard with a P1-button menu
  (paused — display module was defective; ILI9341 replacement pending)
- `v0.5.0` — Lite variant: web-only, WiFiManager network menu, I2C OLED for IP and
  winner, service button removed (settings moved to the web UI)

Conventions: SemVer, conventional commits, architecture discussed before code.

## License

MIT — see [LICENSE](LICENSE).
