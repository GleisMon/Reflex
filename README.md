# Reflex

A multilingual reaction party-game for the ESP8266 (Wemos D1 Mini). Players each
hold a wired push-button; the device springs a sudden signal and the fastest
reaction wins. Two game modes, combo bonuses, persistent scores and a retro web
scoreboard with a live leaderboard you open on a phone.

> The name **Reflex** stays the same in every language — only interface strings
> are translated (EN / DE / UK / RU).

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

## Hardware

- **Board:** Wemos D1 Mini (ESP8266MOD)
- **Buzzer:** passive piezo (square-wave chiptune via hardware Timer1)
- **Buttons:** 4 plain momentary switches to GND (one per player) + 1 service switch
- **Display:** none — the phone is the scoreboard; the buzzer is the start signal

### Pinout

| Function   | Wemos | GPIO | Note |
|------------|-------|------|------|
| Player P1  | D1    | 5    | `INPUT_PULLUP`, switch to GND |
| Player P2  | D2    | 4    | |
| Player P3  | D5    | 14   | |
| Player P4  | D6    | 12   | |
| Service    | D7    | 13   | `INPUT_PULLUP`, switch to GND |
| Buzzer     | D8    | 15   | passive piezo to GND, no pull-up |

No player/service button sits on a boot-strapping pin (GPIO0/D3, GPIO2/D4,
GPIO15/D8), so a button held during reset never breaks boot or enters flash
mode. All buttons use internal pull-ups with software debounce. Free: D3, D4,
D0, RX/TX, A0.

Service switch: short tap = cycle mode · hold ≥3 s + a player button = reset that
player · hold ≥6 s alone = mute. The same controls (plus theme/language/sound
set and a game-mode switch) are available from the web UI.

## Firmware

Arduino IDE, board *LOLIN(WeMos) D1 R2 & mini*. Open `firmware/reflex/reflex.ino`
(the `index_html_gz.h` tab sits next to it and is included automatically).

Libraries (Library Manager, one click each):
- **WebSockets** by Markus Sattler (Links2004 / arduinoWebSockets)
- `ESP8266WebServer`, `LittleFS`, `ESP8266WiFi`, `Ticker` — bundled with the core

The web page is gzip-compressed into `index_html_gz.h` and served from flash;
state and settings travel over WebSocket (port 81).

## Web scoreboard

`web/index.html` is self-contained (no internet/CDN). Display-only — no on-screen
gameplay input. Open it directly for a demo playthrough; on hardware it goes LIVE
over WebSocket. Settings (stored in LittleFS on the device):

- **Languages:** English, Deutsch, Українська, Русский
- **Visual themes:** DOOM, Fallout, Grayscale, Amber, Neon
- **Sound sets:** DOOM, Arcade, Beep, Chip — plus master sound on/off
- **Game mode switch** and a **leaderboard** (rank, score, best reaction in ms)

### Usage
Flash → connect the phone to Wi-Fi **Reflex** → open `http://192.168.4.1`.

## Roadmap

- `v0.1.0` — OLED firmware baseline
- `v0.2.0` — Wi-Fi AP + async web server + WebSocket
- `v0.3.0` — Wemos D1 Mini refactor: Timer1 square-wave audio, WebSocketsServer,
  LittleFS persistence, reaction-time leaderboard, web mode switch (OLED dropped)

Conventions: SemVer, conventional commits, architecture discussed before code.

## License

MIT — see [LICENSE](LICENSE).
