# OUI-Spy C6 — External Hardware Wiring Guide

This document explains how to add **push buttons** and a **piezo buzzer/speaker** to your Waveshare ESP32-C6-LCD-1.47 for standalone operation without a phone.

---

## Overview

The board exposes 15 GPIOs on breadboard headers. The firmware reserves three GPIOs for buttons and one for the buzzer. All buttons use **active-low logic** with internal pull-up resistors — no external resistors needed. The buzzer uses **PWM output** for tone generation.

```
┌──────────────────────────────────────────┐
│  Waveshare ESP32-C6-LCD-1.47 (top view)  │
│                                          │
│  ┌──────────────────────────┐            │
│  │      1.47" IPS LCD       │            │
│  │       172 × 320          │            │
│  └──────────────────────────┘            │
│                                          │
│  Left Header          Right Header       │
│  ┌─────────┐         ┌─────────┐        │
│  │ 5V      │         │ GND     │        │
│  │ GND     │         │ 3.3V    │        │
│  │ GPIO 0  │         │ GPIO 23 │        │
│  │ GPIO 1  │         │ GPIO 20 │        │
│  │ GPIO 2  │         │ GPIO 19 │ ← BTN_BACK
│  │ GPIO 3  │         │ GPIO 18 │ ← BUZZER
│  │ GPIO 4  │         │ GPIO 11 │ ← BTN_ACTION
│  │ GPIO 5  │         │ GPIO 10 │ ← BTN_MODE
│  │ GPIO 9  │ BOOT    │ GPIO 12 │        │
│  └─────────┘         └─────────┘        │
│                                          │
│            [USB-C]                       │
└──────────────────────────────────────────┘
```

---

## Pin Assignments

| Function | GPIO | Header Position | Notes |
|----------|------|-----------------|-------|
| **Mode Button** | 10 | Right header | Cycles through modes |
| **Action Button** | 11 | Right header | Contextual action per mode |
| **Back Button** | 19 | Right header | Returns to mode select |
| **Buzzer/Speaker** | 18 | Right header | PWM tone output |
| **Boot Button** | 9 | Left header (built-in) | Also works as mode cycle |

---

## Button Wiring

Each button needs only **two wires**: one to the GPIO pin and one to **GND**.

### What You Need
- 3× momentary push buttons (any normally-open tactile switch)
- Hookup wire or breadboard jumpers

### Wiring Diagram

```
    GPIO 10 ────┤ ├──── GND     (Mode Button)
    GPIO 11 ────┤ ├──── GND     (Action Button)
    GPIO 19 ────┤ ├──── GND     (Back Button)
    
    ┤ ├ = momentary push button (normally open)
```

**Polarity**: Push buttons have no polarity — wire either leg to GPIO, the other to GND.

### How It Works

The firmware enables the **internal pull-up resistor** on each GPIO pin, so the pin reads HIGH (3.3V) when the button is not pressed. Pressing the button connects the pin to GND, pulling it LOW. The firmware detects this transition and recognizes multi-click plus hold gestures.

- **Single click**: Navigate next / run configured shortcut
- **Double-click**: Navigate previous (or toggle Fox LED mode on Fox main screen)
- **Triple-click**: Back/cancel (or clear Fox target on Fox main screen)
- **Quintuple-click**: In Flock You, jump to Fox Hunter tracking a detected Flock camera
- **Hold (~0.5s)**: Select / confirm
- **Long-hold warning (~3.5s)**: Orange warning flash
- **Long-hold (~5s)**: Return to mode select

### Button Functions

| Button | Default single-click shortcut | Notes |
|--------|-------------------------------|-------|
| **Mode** (GPIO 10) | Next mode | Shortcut is configurable in Settings |
| **Action** (GPIO 11) | Fox LED mode toggle (Fox only) | Shortcut is configurable in Settings |
| **Back** (GPIO 19) | Mode select | Shortcut is configurable in Settings |
| **Boot** (GPIO 9, built-in) | Input button (same gesture rules) | Can navigate/select without external buttons |

Shortcuts are configurable on-device under Settings (`Shortcut BTN10/11/19`).

---

## Buzzer / Speaker Wiring

### Option A: Passive Piezo Buzzer (Recommended)

A passive piezo buzzer generates sound from PWM signals. The firmware outputs tones at various frequencies (440 Hz – 2000 Hz) for melodies and proximity alerts.

**What You Need:**
- 1× passive piezo buzzer (3.3V compatible, 2-pin)
- Hookup wire

**Wiring:**
```
    GPIO 18 ──── (+) Buzzer (─) ──── GND
```

**Polarity**: Most passive piezo buzzers **have polarity** — the longer leg or the leg marked with `+` connects to GPIO 18, the shorter leg to GND. Check the markings on your specific buzzer. If wired backwards, it will still work but at reduced volume.

### Option B: Active Buzzer

An active buzzer contains its own oscillator and produces a fixed tone when powered. The firmware toggles the GPIO to create beep patterns, but you won't hear different frequencies.

**Wiring is the same** as passive:
```
    GPIO 18 ──── (+) Active Buzzer (─) ──── GND
```

### Option C: Small Speaker with Amplifier

For louder output, use a small amplifier module (like a PAM8403) with a speaker:

```
    GPIO 18 ──── AMP Input(+)
    GND     ──── AMP Input(─) / AMP GND
    3.3V    ──── AMP VCC
                 AMP Output ──── Speaker
```

**Important**: Do NOT connect a raw speaker directly to the GPIO pin. Speakers are low-impedance (4Ω-8Ω) and drawing too much current will damage the ESP32. Always use an amplifier module.

### Sound Features

| Mode | Sound | Description |
|------|-------|-------------|
| Boot | ♪ Rising three-note melody | Confirms power-on |
| Flock You | ♪ Unique melody + short beep on new detection | Audible alert for each new device found |
| Fox Hunter | ♪ Variable-rate beeping | Faster = closer to target, pitch increases with signal strength |
| Sky Spy | ♪ Unique melody + tone on new drone | Alert when a new drone enters range |

The buzzer can be **enabled/disabled** from the web interface settings panel. The setting persists across reboots.

---

## Complete Wiring Summary

```
Component          ESP32-C6 Pin    Other Connection
─────────────────────────────────────────────────
Mode Button        GPIO 10         GND
Action Button      GPIO 11         GND
Back Button        GPIO 19         GND
Buzzer (+)         GPIO 18         —
Buzzer (−)         —               GND
```

Total: **4 signal wires + shared GND connections**. All connections go to the **right header** of the board.

---

## Breadboard Layout Example

```
       ┌─────────────────────────────────────┐
       │      ESP32-C6-LCD-1.47              │
       │     (inserted in breadboard)         │
       └──┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┘
          │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │
          5V G 0 1 2 3 4 5 9  G 3V 23 20 19 18 11 10 12 13
          
     Connect:
       Pin 10 ──[button]── GND rail
       Pin 11 ──[button]── GND rail
       Pin 19 ──[button]── GND rail
       Pin 18 ──[buzzer+]
       GND rail ──[buzzer-]
```

---

## Available GPIO Pins (Unused by this firmware)

If you want to add more hardware, these GPIOs are completely free:

| GPIO | ADC | Suggested Use |
|------|-----|---------------|
| 0 | ADC1_CH0 | Analog sensor |
| 1 | ADC1_CH1 | Analog sensor |
| 2 | ADC1_CH2 | Analog sensor |
| 3 | ADC1_CH3 | Analog sensor |
| 20 | — | I2C SCL (with external sensor) |
| 23 | — | I2C SDA (with external sensor) |

GPIO 4 and GPIO 5 are shared with the SD card slot — available when no SD card is inserted.

GPIO 12 and GPIO 13 are USB D-/D+ — avoid using these for other purposes unless you're certain USB is not needed.

---

## Troubleshooting

**Buttons not responding?**
- Verify you're connecting to GND (not 3.3V or 5V)
- Check that the button is normally-open (NO), not normally-closed (NC)
- The built-in BOOT button on GPIO 9 always works — test with that first

**No sound from buzzer?**
- Confirm polarity (+ to GPIO 18, - to GND)
- Check that sound is enabled in the web UI settings
- Try swapping to a different buzzer — some require 5V (this outputs 3.3V)

**Buzzer is quiet?**
- Passive buzzers at 3.3V are inherently quiet — consider adding a small amplifier
- Active buzzers are typically louder

**GPIO 15 warning on boot?**
- GPIO 15 (LCD DC pin) is a strapping pin. The firmware handles this correctly; the warning in serial output is normal and can be ignored.
