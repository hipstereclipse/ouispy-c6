# OUI-Spy C6

**Official ESP32-C6 RF intelligence firmware for the Waveshare ESP32-C6-LCD-1.47**

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform: ESP32-C6](https://img.shields.io/badge/Platform-ESP32--C6-0a7ea4)
![Framework: ESP-IDF](https://img.shields.io/badge/Framework-ESP--IDF%20v5.3.x-222)

> Production-oriented passive RF situational awareness toolkit for BLE surveillance hardware discovery, proximity tracking, and Remote ID drone detection.

> This project is a from-scratch C rewrite and hardware-specific spinoff of [**OUI-Spy Unified Blue**](https://github.com/colonelpanichacks/oui-spy-unified-blue) by [ColonelPanicHacks](https://github.com/colonelpanichacks). The original project pioneered the concept of passive BLE surveillance hardware detection and drone monitoring on ESP32 — this version ports and extends those ideas natively to the ESP32-C6 platform with a custom display UI, hardware buzzer integration, and a modern web interface.

OUI-Spy C6 consolidates three passive RF intelligence modes into a single flash image running on the Waveshare ESP32-C6-LCD-1.47 dev board. Pure ESP-IDF C, no Arduino — just bare metal and radio waves.

Current firmware version: 0.41.00

## Quick Links

- [Build & Flash](#build--flash)
- [Web Interface](#web-interface)
- [API Endpoints](#api-endpoints)
- [Hardware Wiring Guide](HARDWARE.md)
- [License](LICENSE)

---

## Modes

### Flock You — Surveillance Hardware Detector

Passively scans for Flock Safety ALPR cameras and ShotSpotter Raven acoustic sensors using BLE advertisements.

- **5 detection heuristics**: OUI prefix matching (20 known prefixes), device name pattern matching, Manufacturer Company ID (0x09C8 / XUNTONG), Raven GATT service UUID fingerprinting, and firmware version estimation
- Real-time scrollable device list on the LCD showing RSSI, hit count, and detection method
- Flock LCD now shows live `GPS TAG` status (`ON`/`OFF`) so operators can verify geotagging state at a glance
- New detections trigger an orange LED warning flash, then transition to a breathing red/purple heartbeat whose intensity tracks signal strength
- Idle sweep alternates a dim amber/purple heartbeat so low activity remains visible
- Tracks up to 200 unique devices with MAC deduplication
- CSV export of all detections via web UI

### Fox Hunter — BLE Proximity Tracker

Locks onto a single BLE MAC address and acts as a Geiger counter for Bluetooth — the buzzer beeps faster and the display updates as you physically close in on the target.

- **7-zone RSSI-to-cadence mapping**: from 15ms (machine gun, >-35 dBm) down to 800ms (slow pulse, <-85 dBm)
- Color-mapped proximity bar and signal strength visualization on LCD
- Lost-signal screen waits a short grace period (about 9 seconds) after last contact before switching to full "SIGNAL LOST"
- Targets can be set via the web UI, or imported directly from Flock You detections with a single button hold
- Target MAC persists across reboots via NVS flash storage
- Saved target registry behaves like a compact contact list in the web UI, with live `last seen`, source, and RSSI context when the device is visible in the current session
- Fox registry capacity automatically expands from 8 entries to 32 when a usable microSD card is mounted
- Two toggleable LED tracking modes:

  **Detector Mode:**
  - Solid **orange** LED when no target is set (standing by)
  - Solid **green** LED once a target is selected (hunting)
  - **Blinking red** when the target is detected — blink rate is proportional to signal strength (slow blinks = far away, rapid blinking = right on top of it)

  **Sting Mode:**
  *"...and the light of Eärendil shone upon it." One does not simply walk into range without being noticed.*
  - LED off when no target is set or signal is lost
  - Solid **blue** glow when the target is detected, with brightness proportional to signal strength — dim blue when far, blazing bright blue at close range

  Toggle between Detector and Sting with a double-click while in Fox Hunter mode, or via the web UI toggle button.

### Sky Spy — Drone Detector

Dual-protocol passive drone detection with a Naval CIC-inspired radar display.

- **ASTM F3411 Remote ID** detection via WiFi NAN action frames and BLE Service Data (UUID 0xFFFA)
- **DJI proprietary DroneID** detection via beacon vendor IEs and BLE Company ID (0x2CA3)
- **Parrot** detection via OUI prefix matching
- Parses OpenDroneID message types 0–5: Basic ID, Location, Authentication, Self-ID, System, and Operator ID
- Displays drone serial number, position, altitude, speed, pilot location, and operator info
- Animated phosphor-green radar with rotating sweep, concentric range rings, cardinal labels, and fading trail
- Detected drones appear as amber blips on the radar, positioned by RSSI (distance) and MAC hash (angle) with a blinking animation
- Contact list sidebar with protocol color bars (green = ASTM, red = DJI)
- Green breathing LED while scanning, with orange flash on new drone ping
- Tracks up to 16 simultaneous drones by default, and up to 64 when a usable microSD card is available
- 30-second automatic expiry for stale drone contacts

---

## Web Interface

A modern single-page dark-themed web UI served directly from the device. Connect to the device's WiFi AP and open **https://192.168.4.1** (preferred) or **http://192.168.4.1** in any browser.

For phone GPS features, use HTTPS. The device uses a local self-signed certificate, so your browser may show a one-time certificate warning before allowing secure-context APIs.
If you use HTTP on iPhone Safari, GPS remains unavailable and the UI keeps GPS state OFF.

- Real-time state updates via WebSocket (500ms push interval)
- Mode switching with animated tab navigation
- Per-mode device lists, proximity visualization, and drone cards
- Fox Hunter target selection and LED mode toggle
- Flock You GPS ON/OFF safety toggle; GPS marking only when enabled and secure (HTTPS)
- GPS toggle is synchronized to firmware state (`gpsTagging`) so web + device stay in sync
- GPS status is now surfaced across the web interface and in the on-device Settings screen, not only in Flock You
- Settings panel: LCD brightness, AP broadcast visibility, single AP naming (UniSpy-C6), LED color palette, sound profiles, GPS tagging, button shortcut mappings, and microSD logging controls with per-source filters
- The on-device Settings screen now uses grouped submenus for Connectivity, Display, Sound Profiles, Button Shortcuts, Logging, and Maintenance so you can reach related options without scrolling one flat list
- microSD status is surfaced as `Available`, `Needs Format`, or `Not Found` in both the LCD UI and web UI
- This project vendors the ESP-IDF `fatfs` component locally with `FF_FS_EXFAT` enabled, so compatible microSD cards formatted as exFAT can mount without reflashing a custom IDF toolchain
- microSD usage now updates live as logs accumulate, including used/total card capacity in both the LCD and web UI
- Unformatted but detected cards can be formatted directly from the on-device Settings menu
- When microSD logging is enabled, identity-bearing log records (entries containing device MAC/unique IDs) are written to a protected log file and are not auto-pruned; if storage gets tight, the oldest non-critical event records are trimmed first
- Fox Hunter now uses a compact searchable device console that merges saved devices, WiFi clients, Flock devices, Sky drones, and Fox BLE detections into one view with profile editing and pin actions
- CSV data export for Flock You detections
- Mobile-optimized responsive embedded web UI

### Offline Map Addon

The shared pinned-device map can render offline slippy-map tiles for saved Flock, Fox, and Sky locations.

- Shared map points now include the browser/device current location while GPS is live, or the last known browser/device location when a recent fix exists
- Browser GPS map points are mirrored into the device's local LCD shared-map view so the on-device map no longer depends only on manually saved pins

- Requirements:
  - Open the device over `https://192.168.4.1`
  - Enable `GPS Tagging` in Settings or the Flock page toggle
  - Allow browser location access so GPS status becomes live
  - Save device pins using GPS or manual coordinates from the Flock, Fox, or Sky views
- Access:
  - In the Flock You or Sky Spy web UI, triple-click the GPS status tile to open the shared map panel
  - Double-click the map canvas to cycle zoom levels
- Tiles:
  - Preferred device-backed path: copy a `map` folder onto the microSD card at `/sdcard/map` using standard slippy-map layout: `z/x/y.png`, `jpg`, `jpeg`, or `webp`
  - In the web UI, `Load Device/Folder Tiles` first checks `/sdcard/map` served by the device itself, even on browsers that also support local folder selection
  - Browsers that support directory selection can load either the `map` folder itself or a parent folder that contains `map`, using the same `z/x/y.ext` layout
  - If no offline tiles are loaded, the map still shows a gray grid with relative pinned devices
- Notes:
  - Browser directory selection is no longer required when `/sdcard/map` is present on the device
  - The firmware now serves tiles directly through `/api/map/tile`, so the embedded map remains usable even in browsers that cannot open local folders
  - The firmware also tolerates a nested `/sdcard/sdcard/map` layout so flasher-exported folders copied one level too deep still work
  - The browser map can render `png`, `jpg`, `jpeg`, or `webp` tiles
  - The on-device LCD renderer can center and browse any indexed tile cache, but image tile drawing on the LCD still requires PNG tiles under `/sdcard/map`; `jpg`/`jpeg`/`webp` tile sets fall back to the grid view instead of blocking the map entirely
  - Pins come from locations you saved in the web UI, so populate those first for a meaningful map

### Fox Hunter Web Console

Fox Hunter now uses a compact merged device console instead of separate source lists buried below the fold.

- Saved Fox devices, WiFi clients, Flock devices, Sky drones, and Fox BLE detections are merged into one searchable directory
- Filter chips let you narrow the list to priority, saved, live, nearby, following, WiFi, Flock, drone, BLE, or all devices
- Selecting a device opens a profile panel with:
  - saved notes and section overrides
  - pinned GPS/manual coordinates
  - current Fox fused GPS estimate when the selected device is the active Fox target
  - one-click track, GPS mark, manual mark, save, and delete actions

---

## Hardware

### LED Behavior Summary

| Mode | LED State |
|------|-----------|
| Boot / Init | Phase-colored init sequence |
| Mode Select | Solid green |
| Flock You (idle) | Dim amber/purple heartbeat |
| Flock You (devices found) | Breathing purple |
| Fox Hunter — Detector (no target) | Solid orange |
| Fox Hunter — Detector (target set, searching) | Solid green |
| Fox Hunter — Detector (target detected) | Blinking red (speed = proximity) |
| Fox Hunter — Sting (no target / searching) | Off |
| Fox Hunter — Sting (target detected) | Solid blue (brightness = proximity) |
| Sky Spy | Breathing green |
| Reset warning (hold 3.5s) | Triple orange flash |

### On-Device Controls

- **Click**: Navigate to next item
- **Double-click**: Navigate to previous item
- **Triple-click**: Back/cancel (Fox main: clears target; Fox registry: exits registry)
- **Settings menu now includes a GPS Tagging toggle**, so the device-side UI can enable/disable web GPS capture without opening the browser
- **Quintuple-click**:
  - In Flock You: lock selected/strongest Flock camera and jump to Fox Hunter
  - In Sky Spy: lock selected/strongest drone and jump to Fox Hunter
- **Hold 0.5s**: Select / activate current item
- **Hold 3.5s**: Orange LED warning flash (reset imminent)
- **Hold 5s**: Return to mode selector

In operational modes, single-click can trigger per-button shortcuts (configurable in Settings). Default shortcuts are: BTN10 next mode, BTN11 Fox LED mode toggle (in Fox), BTN19 mode select.

### Hardware Requirements

- **Waveshare ESP32-C6-LCD-1.47** (ASIN B0DK5J6LX3)
- USB-C cable for power and flashing
- Optional: 3 push buttons + piezo buzzer/speaker (see [HARDWARE.md](HARDWARE.md))

---

## WiFi Access Points

Each mode can run its own dedicated WiFi AP, or you can enable a unified AP name in Settings:

| Mode | SSID | Password | Channel |
|------|------|----------|---------|
| Mode Select | `ouispy-c6` | `ouispy123` | 6 |
| Flock You | `flockyou-c6` | `flockyou123` | 6 |
| Fox Hunter | `foxhunt-c6` | `foxhunt123` | 6 |
| Sky Spy | `skyspy-c6` | `skyspy1234` | 6 |

When `Single AP Name` is enabled, all modes use `UniSpy-C6` with password `ouispy123`.

All modes use channel 6 for optimal alignment with Remote ID NAN detection.

---

## Build & Flash

### Prerequisites

- [ESP-IDF v5.3.2](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32c6/get-started/) on Windows
- Python 3.11 for the ESP-IDF build environment

### microSD Filesystem Note

- This firmware mounts FAT12/16/32 and exFAT volumes through a vendored copy of ESP-IDF's `fatfs` component
- The local override enables `FF_FS_EXFAT` so exFAT-formatted microSD cards can mount on ESP-IDF 5.3.x without modifying the global IDF installation
- If the card still reports `Needs Format`, the filesystem is likely damaged or uses a format other than FAT/exFAT

### Build

```bash
cd ouispy-c6
idf.py set-target esp32c6
idf.py build
```

### One-Step Flash + Monitor

```bash
python flash.py --port COMx
```

`flash.py` can first offer downloading the latest prebuilt firmware release (no local build required), or build from source, then flashes the board, starts a serial monitor, and saves timestamped crash logs under `logs/`.

### Manual Flash

```bash
# If the board doesn't enter flash mode automatically:
# Hold BOOT → press RESET → release RESET → release BOOT
idf.py -p COMx flash monitor
```

Replace `COMx` with your serial port (e.g., `COM3` on Windows, `/dev/ttyACM0` on Linux).

### First Boot

1. LCD shows the gold & purple OUI-SPY boot splash with startup melody
2. Device starts a WiFi AP (`ouispy-c6` / `ouispy123` by default, or `UniSpy-C6` if unified AP naming is enabled)
3. Connect your phone or laptop to the AP
4. Open **http://192.168.4.1** in a browser
5. Select a mode from the on-screen selector or web UI

---

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Web UI |
| GET | `/api/state` | Full JSON state (mode, heap, uptime, settings, fox target, gpsTagging, microSD status) |
| GET | `/api/devices` | Flock You device list (JSON array) |
| GET | `/api/drones` | Sky Spy drone list (JSON array) |
| GET | `/api/export/csv` | Download Flock detections as CSV |
| POST | `/api/mode` | Change mode: `{"mode": 0-3}` |
| POST | `/api/fox/target` | Set Fox target: `{"mac":"AA:BB:CC:DD:EE:FF"}` or `{"index":0}` |
| POST | `/api/fox/ledmode` | Set/toggle Fox Hunter LED mode (`{"mode":0|1}` sets explicitly; empty body toggles) |
| POST | `/api/settings` | Update prefs: `{"brightness":200,"sound":true,"led":true,"gpsTagging":false}` |
| WS | `/ws` | WebSocket for real-time push updates |

`/api/state` now includes `microsdAvailable`, `microsdStatus`, and `foxRegistryCapacity`, where `microsdStatus` is one of `Available`, `Needs Format`, or `Not Found`.

---

## Architecture

```
main.c              — Boot flow, mode transitions, main loop
app_common.c/h      — Shared types, global state, utility functions
display.c/h         — ST7789V3 LCD driver (esp_lcd + LEDC backlight)
led_ctrl.c/h        — WS2812 RGB LED with sine-wave breathing (RMT backend)
buzzer.c/h          — Piezo buzzer via LEDC PWM
button.c/h          — Multi-gesture button state machine (click, double-click, triple-click, quintuple-click, hold, long-hold)
nvs_store.c/h       — NVS persistent storage for mode + preferences
wifi_manager.c/h    — SoftAP management with per-mode SSID
ble_scanner.c/h     — NimBLE BLE scanner
sniffer.c/h         — WiFi promiscuous sniffer for drone detection
flock_you.c/h       — Flock Safety / ShotSpotter Raven detector
fox_hunter.c/h      — BLE proximity tracker with buzzer cadence + dual LED modes
sky_spy.c/h         — ASTM F3411 + DJI drone detector with radar UI
web_server.c/h      — HTTP + WebSocket server (esp_http_server)
index.html          — Single-page web UI (Tailwind dark theme, embedded in flash)
font5x7.h           — Minimal 5×7 bitmap font for LCD text rendering
```

### Radio Coexistence

The ESP32-C6 shares a single 2.4 GHz radio between WiFi and BLE via time-division multiplexing:
- **WiFi AP**: Fixed on channel 6 (aligned with NAN Remote ID)
- **BLE Scan**: 50ms window / 100ms interval (50% duty cycle)
- **Promiscuous mode**: Management frame filter with queue-based processing

### Memory Budget

Target profile on ESP32-C6FH4 (512KB SRAM, 4MB/8MB Flash, no PSRAM):
- Stack: ~28KB across all tasks
- Device tracking: ~50KB (200 × 252 bytes)
- Drone tracking: ~6KB (16 × 384 bytes)
- Display buffers: DMA-allocated, freed after each draw operation
- Web UI: Embedded in flash, zero RAM overhead at rest

---

## Project Structure

```
ouispy-c6/
├── CMakeLists.txt          — Top-level project cmake
├── flash.py                — Build, flash, and serial-monitor helper
├── flash.bat               — Windows wrapper for flash.py
├── HARDWARE.md             — Wiring guide for buttons and buzzer
├── partitions.csv          — Custom partition table (3.4MB app + 512KB storage)
├── sdkconfig.defaults      — ESP-IDF config defaults
├── logs/                   — Timestamped serial monitor captures
└── main/
    ├── CMakeLists.txt      — Component cmake with EMBED_TXTFILES
    ├── idf_component.yml   — Managed component dependencies (led_strip v3.0.3)
    ├── main.c
    ├── app_common.c/h
    ├── display.c/h
    ├── font5x7.h
    ├── led_ctrl.c/h
    ├── buzzer.c/h
    ├── button.c/h
    ├── nvs_store.c/h
    ├── wifi_manager.c/h
    ├── ble_scanner.c/h
    ├── sniffer.c/h
    ├── flock_you.c/h
    ├── fox_hunter.c/h
    ├── sky_spy.c/h
    ├── web_server.c/h
    └── index.html
```

---

## License

MIT License. See [LICENSE](LICENSE) and individual source file SPDX identifiers.

---

## Acknowledgments

- **[ColonelPanicHacks](https://github.com/colonelpanichacks)** — Original [OUI-Spy Unified Blue](https://github.com/colonelpanichacks/oui-spy-unified-blue) project. This firmware is a spinoff and from-scratch C rewrite of their work.
- [opendroneid-core-c](https://github.com/opendroneid/opendroneid-core-c) reference library
- Waveshare for the ESP32-C6-LCD-1.47 hardware platform
- Espressif ESP-IDF framework and NimBLE stack
