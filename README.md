# Now Playing Display

A desk display that shows what is currently playing on a Windows PC — album art, track title, artist, and a progress bar — on a 240×240 TFT driven by an ESP32, updating in near real time.

It works with **any app that reports to the Windows media session API**: Spotify, YouTube in a browser, VLC, and more. No Spotify API key or per-app integration needed.

**Measured performance:** ~120 ms from a track change on the PC to the updated track appearing on the display.

## How it works

```
 Windows PC                                        ESP32
┌───────────────────────────┐   WebSocket    ┌────────────────────┐
│ now_playing_server.py     │ ────────────➜ │ NowPlayingDisplay  │
│                           │   port 8765    │ .ino (LVGL UI)     │
│ • WinRT media session API │                │                    │
│ • Sends track metadata    │                │ • Renders art,     │
│   as JSON (media_update)  │                │   title, artist,   │
│ • Converts album art to   │                │   progress bar     │
│   RGB565 and streams it   │                │ • Optional spinning│
│   in 8 KB binary chunks   │                │   "vinyl" mode     │
└───────────────────────────┘                └────────────────────┘
```

- **`media_update` (JSON, 1/sec):** title, artist, album, volume, playing state, position and duration in seconds.
- **`imageSpecs` (JSON) + binary frames:** the server resizes the album art, converts it pixel-by-pixel to RGB565, and streams it in 8 KB chunks, which the ESP32 reassembles into the LVGL image buffer.
- **Setup handshake:** on connect, the ESP32 reports its background color and display mode, so the server can blend the spinning-disc artwork correctly.
- **Character cleanup:** Windows track titles contain characters the LVGL font can't render (curly quotes, em dashes, accented letters, symbols). The server maps them to safe ASCII equivalents before sending — see `CHARACTER_REPLACEMENTS` in the server.

## Hardware

- ESP32 development board
- 240×240 TFT display (TFT_eSPI-compatible), backlight on GPIO 21
- USB cable for power/programming

## Firmware setup (ESP32)

1. Install the ESP32 board package in the Arduino IDE.
2. Install libraries via Library Manager: **TFT_eSPI**, **LVGL**, **ArduinoJson**, **WebSocketsClient** (Markus Sattler).
3. Configure TFT_eSPI for your display in its `User_Setup` file (driver + pins).
4. In your LVGL config (`lv_conf.h`), enable the Montserrat 10 and 14 fonts.
5. Copy `firmware/NowPlayingDisplay/config.example.h` to `config.h` and fill in your Wi-Fi credentials and your PC's local IP address. `config.h` is gitignored — never commit your real credentials.
6. Open `firmware/NowPlayingDisplay/NowPlayingDisplay.ino`, upload, and the display will attempt to connect.

## PC server setup (Windows)

> Windows only — the server relies on the WinRT media session API and pycaw.

1. Install Python 3.11+.
2. `pip install -r pc_server/requirements.txt`
3. Run: `python pc_server/now_playing_server.py`
4. The server listens on port 8765 and waits for the ESP32 to connect. Play something on the PC and it appears on the display.

## Project structure

```
now-playing-display/
├── README.md
├── LICENSE
├── pc_server/
│   ├── now_playing_server.py   # WinRT -> WebSocket server
│   └── requirements.txt
└── firmware/
    └── NowPlayingDisplay/
        ├── NowPlayingDisplay.ino
        ├── placeHolderImage.c  # LVGL image asset shown when nothing is playing
        └── config.example.h    # copy to config.h with your values
```

## Known issues

- Switching between square (1:1) and non-square album art can misplace the cover offset on the display.
- The WebSocket server has no authentication by design — it is meant to run on a trusted home LAN only. Don't expose port 8765 to the internet.

## Possible future work

- On-device playback/volume control — the server already includes pycaw-based volume helpers (`setVolume`, `mute`, `getVolume`) that aren't wired to device input yet.
- Pause/play state indicator polish on the display.

## License

MIT — see [LICENSE](LICENSE).
