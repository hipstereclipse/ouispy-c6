# OUI-Spy C6

**Open-source RF intelligence firmware for the Waveshare ESP32-C6-LCD-1.47**

OUI-Spy C6 is a native ESP-IDF C port of the OUI-Spy Unified Blue project, built specifically for the Waveshare ESP32-C6-LCD-1.47 development board. It consolidates three passive RF intelligence modes into a single flash image with a modern web-based interface and on-device display.

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)

---

## Features

### Flock You — Surveillance Hardware Detector
- Passive BLE scanner identifying Flock Safety ALPR cameras and ShotSpotter Raven sensors
- **5 detection heuristics**: OUI prefix matching (20 prefixes), device name patterns, manufacturer Company ID (0x09C8), Raven GATT UUID fingerprinting, and firmware version estimation
- Real-time device list with RSSI, hit count, and detection method indicators
- New detections flash the RGB LED orange as a warning, then transition into a red heartbeat whose intensity and tempo track signal strength
- Tracks up to 200 unique devices with deduplication by MAC
- CSV export of all detections

### Fox Hunter — Proximity Tracker
- Locks onto a single BLE MAC and maps RSSI to a variable-rate buzzer cadence
- Functions like a Geiger counter for Bluetooth — beeps faster as you get closer
- 7-zone RSSI-to-cadence mapping (-25 dBm = machine gun, below -85 dBm = slow pulse)
- Targets can be imported directly from Flock You detections with one tap
- Visual proximity bar with color gradient (red→yellow→green)
- Persists target across reboots via NVS

### Sky Spy — Drone Detector
- Dual-protocol ASTM F3411 Remote ID detection (WiFi NAN + BLE)
- DJI proprietary DroneID detection (beacon vendor IEs + BLE Company ID 0x2CA3)
- Parses OpenDroneID message types: Basic ID, Location, System, Operator ID
- Displays drone position, altitude, speed, pilot location, and operator ID
- Animated radar UI when scanning, detailed drone cards when detected
- Tracks up to 16 simultaneous drones with automatic expiry

### Web Interface
- Modern single-page application served directly from the device
- Real-time updates via WebSocket (500ms push interval)
- Mode switching, device lists, proximity visualization, drone cards
- Settings panel (brightness, sound, LED toggle)
- CSV data export
- Mobile-optimized responsive design

### Hardware Integration
- **172×320 IPS LCD**: Safe-area-aware headers and footers, improved contrast, device lists, proximity indicators per mode
- **WS2812 RGB LED**: Mode-aware alerts including Flock warning flashes + heartbeat tracking, Fox Hunter strength feedback, and Sky Spy scan pulse
- **Buzzer/Speaker**: Distinct startup melodies per mode, proximity cadence, detection alerts
- **Physical Buttons**: Click/double-click navigation, 1-second select hold, 5-second reset warning flash, 7-second return-to-selector hold

---

## Hardware Requirements

- **Waveshare ESP32-C6-LCD-1.47** (ASIN B0DK5J6LX3)
- USB-C cable for power and flashing
- Optional: Push buttons, piezo buzzer/speaker (see [HARDWARE.md](HARDWARE.md))

---

## Build & Flash

### Prerequisites

- [ESP-IDF v5.3.2](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32c6/get-started/) on Windows
- Python 3.11 for the ESP-IDF environment on this project

### Build

```bash
cd ouispy-c6
idf.py set-target esp32c6
idf.py build
```

On Windows, this project has been validated with the ESP-IDF export script and a Python 3.11 environment.

### One-Step Flash + Monitor

```bash
python flash.py --port COMx
```

`flash.py` builds the firmware, flashes the board, starts a serial monitor, and saves crash logs under `logs/`.

### Flash

```bash
# Hold BOOT → press RESET → release RESET → release BOOT
idf.py -p COMx flash monitor
# Press RESET after flashing to run
```

Replace `COMx` with your serial port (e.g., `COM3` on Windows, `/dev/ttyACM0` on Linux).

### On-Device Controls

- `Click`: move to the next item
- `Double-click`: move to the previous item
- `Hold 1s`: select the current item
- `Hold 5s`: LED warning that reset-to-selector is imminent
- `Hold 7s`: return to mode selector

### First Boot

1. The LCD shows the OUI-SPY boot splash
2. The device starts a WiFi access point
3. Connect your phone/laptop to the AP:
   - **SSID**: `ouispy-c6`
   - **Password**: `ouispy123`
4. Open a browser to **http://192.168.4.1**
5. Select a mode from the web interface or press the mode button

---

## WiFi Access Points Per Mode

| Mode | SSID | Password | Channel |
|------|------|----------|---------|
| Mode Select | `ouispy-c6` | `ouispy123` | 6 |
| Flock You | `flockyou-c6` | `flockyou123` | 6 |
| Fox Hunter | `foxhunt-c6` | `foxhunt123` | 6 |
| Sky Spy | `skyspy-c6` | `skyspy1234` | 6 |

All modes use channel 6 for optimal Remote ID NAN detection alignment.

---

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Web UI |
| GET | `/api/state` | Full JSON state (mode, heap, uptime, settings, fox target) |
| GET | `/api/devices` | Flock You device list (JSON array) |
| GET | `/api/drones` | Sky Spy drone list (JSON array) |
| GET | `/api/export/csv` | Download Flock detections as CSV |
| POST | `/api/mode` | Change mode: `{"mode": 0-3}` |
| POST | `/api/fox/target` | Set Fox target: `{"mac":"AA:BB:CC:DD:EE:FF"}` or `{"index":0}` |
| POST | `/api/settings` | Update prefs: `{"brightness":200,"sound":true,"led":true}` |
| WS | `/ws` | WebSocket for real-time push updates |

---

## Architecture

```
main.c              — Boot, mode transitions, main loop
app_common.c/h      — Shared types, state, utility functions
display.c/h         — ST7789V3 LCD driver (esp_lcd + LEDC backlight)
led_ctrl.c/h        — WS2812 RGB LED (RMT, no DMA on C6)
buzzer.c/h          — Piezo buzzer via LEDC PWM
button.c/h          — GPIO buttons with debounce + long press
nvs_store.c/h       — NVS persistent storage
wifi_manager.c/h    — SoftAP management
ble_scanner.c/h     — NimBLE BLE scanner
sniffer.c/h         — WiFi promiscuous sniffer for drone detection
flock_you.c/h       — Flock Safety / Raven detector
fox_hunter.c/h      — BLE proximity tracker with buzzer cadence
sky_spy.c/h         — ASTM F3411 + DJI drone detector
web_server.c/h      — HTTP + WebSocket server (esp_http_server)
index.html          — Single-page web UI (embedded in flash)
font5x7.h           — Minimal bitmap font for LCD text
```

### Radio Coexistence

The ESP32-C6 shares a single 2.4 GHz radio between WiFi and BLE via time-division multiplexing. The firmware uses:
- **WiFi AP**: Fixed on channel 6 (aligned with NAN Remote ID)
- **BLE Scan**: 50ms window / 100ms interval (50% duty) for balanced coexistence
- **Promiscuous mode**: Management frame filter, queue-based processing

### Memory Budget

Target profile on ESP32-C6FH4 (512KB SRAM, 4MB Flash, no PSRAM):
- Stack: ~28KB across all tasks
- Device tracking: ~50KB (200 × 252 bytes)
- Drone tracking: ~6KB (16 × 384 bytes)
- Display buffers: DMA-allocated, freed after each draw
- Web UI: Embedded in flash, zero RAM overhead at rest

---

## Project Structure

```
ouispy-c6/
├── CMakeLists.txt          — Top-level project cmake
├── flash.py                — Build, flash, and serial-monitor helper
├── flash.bat               — Windows wrapper for flash.py
├── logs/                   — Timestamped serial monitor captures
├── partitions.csv          — Custom partition table (3.4MB app + 512KB storage)
├── sdkconfig.defaults      — ESP-IDF config defaults
├── HARDWARE.md             — Wiring guide for external hardware
├── README.md               — This file
└── main/
    ├── CMakeLists.txt      — Component cmake with EMBED_TXTFILES
    ├── idf_component.yml   — Managed component dependencies
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

MIT License. See individual source files for SPDX identifiers.

---

## Acknowledgments

- Original [OUI-Spy Unified Blue](https://github.com/colonelpanichacks/oui-spy-unified-blue) by ColonelPanicHacks
- [opendroneid-core-c](https://github.com/opendroneid/opendroneid-core-c) reference library
- Waveshare for the ESP32-C6-LCD-1.47 hardware platform
- Espressif ESP-IDF framework
