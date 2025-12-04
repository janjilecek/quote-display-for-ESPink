# ESP32 E-Paper Quote Display

A battery-powered daily quote display using an ESP32 and e-Paper screen. Loads quotes from an SD card, displays them randomly without repeating, and can read quotes aloud via button press.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-green.svg)

## Features

- **Daily Quote Display** — Shows a new random quote every 24 hours
- **No Repeats** — Cycles through all quotes before any repeat
- **Audio Playback** — Press the button to hear the current quote read aloud
- **Ultra-Low Power** — Deep sleep between updates for months of battery life
- **Automatic Font Sizing** — Fits quotes to screen with optimal readability
- **Battery Monitoring** — On-screen indicator with low-battery protection
- **Persistent Memory** — Tracks shown quotes across power cycles
- **Time Sync** — NTP sync on first boot, then runs offline

## Hardware

| Component | Specification |
|-----------|---------------|
| Board | [LaskaKit ESPink-Shelf-2.13]([https://www.laskakit.cz/](https://www.laskakit.cz/laskakit-espink-shelf-213-esp32-e-paper/)) |
| Display | GDEY0213B74 (250×128 pixels, 2.13") |
| Battery | 3.7V LiPo (2500mAh recommended) |
| Storage | MicroSD card (FAT32) |

### Pin Configuration

| Function | GPIO |
|----------|------|
| E-Paper DC | 17 |
| E-Paper RST | 16 |
| E-Paper BUSY | 4 |
| Display Power | 2 |
| SD CS | 15 |
| SD MOSI | 23 |
| SD MISO | 19 |
| SD SCK | 18 |
| Wake Button | 32 |
| Audio DAC | 25 (DAC1) |
| Battery ADC | 34 |

## SD Card Structure

```
/
├── quotes.json       # Quote database
└── wavs/
    ├── 1.wav         # Audio for quote ID 1
    ├── 2.wav         # Audio for quote ID 2
    └── ...
```

### quotes.json Format

```json
{
  "quotes": [
    {
      "id": 1,
      "text": "The only way to do great work is to love what you do.",
      "author": "Steve Jobs"
    },
    {
      "id": 2,
      "text": "In the middle of difficulty lies opportunity.",
      "author": "Albert Einstein"
    }
  ]
}
```

### Audio Requirements

- Format: WAV (PCM)
- Sample Rate: 8000–22050 Hz recommended
- Bit Depth: 8-bit or 16-bit
- Channels: Mono
- Filename: `{quote_id}.wav`

## Installation

### Prerequisites

- [Arduino IDE](https://www.arduino.cc/en/software) or PlatformIO
- ESP32 board support package

### Required Libraries

Install via Arduino Library Manager:

- `GxEPD2` — E-Paper display driver
- `ArduinoJson` — JSON parsing
- `Preferences` — Built-in (ESP32 NVS storage)

### Configuration

Edit these constants in the source file:

```cpp
// WiFi (only used once on new upload for time sync)
const char* WIFI_SSID = "YourNetwork";
const char* WIFI_PASS = "YourPassword";

// Timezone
const long GMT_OFFSET = 0;        // Seconds from UTC
const int DAYLIGHT_OFFSET = 0;    // DST offset in seconds

// Display interval (default: 24 hours)
#define REFRESH_INTERVAL (24 * 60 * 60)
```

### Upload

1. Connect the board via USB
2. Select board: **ESP32 Dev Module**
3. Upload the sketch
4. Insert SD card with quotes and audio files

## How It Works

### Wake Sources

| Trigger | Action |
|---------|--------|
| Timer (24h) | Display next random quote |
| Button Press | Play audio for current quote |
| Power On | Initialize, sync time, display quote |

### Quote Cycling

1. On each timer wake, a random unshown quote is selected
2. Shown quote IDs are stored in NVS (persists through power loss)
3. When all quotes are shown, the cycle resets
4. The last quote of a cycle won't be first in the next cycle

### Power Management

- Display powers off completely between updates
- ESP32 enters deep sleep (~10µA)
- WiFi only activates once after new code upload
- Battery voltage checked on every wake
- Critical battery triggers protective shutdown

## Battery Life Estimation

With a 2500mAh battery and 24-hour refresh interval:

| State | Current | Duration | Daily mAh |
|-------|---------|----------|-----------|
| Deep Sleep | ~10µA | 23h 59m | ~0.24 |
| Active (display) | ~80mA | ~3s | ~0.07 |
| Active (audio) | ~100mA | ~10s | ~0.28 |

**Estimated battery life: 4–6 months** (varies with audio usage)

## Stats Logging

Every 30 quotes, stats are saved to the SD card:

```
=== Quote Display Stats ===
Timestamp: 2025-01-15_08-30-00
Build: Jan 15 2025 08:00:00

Total Quotes Displayed: 30
Current Quote ID: 42

Battery Voltage: 3.85 V
Battery Percent: 71 %

Quotes in current cycle: 30/100
```

## Troubleshooting

| Issue | Solution |
|-------|----------|
| "SD Card Error" | Check card is FAT32, properly inserted |
| "JSON Scan Error" | Validate quotes.json format |
| No audio | Verify WAV format, check filename matches ID |
| Display not updating | Check battery voltage, verify deep sleep wake |
| Time incorrect | Ensure WiFi credentials are correct for initial sync |

## Memory Optimization

This project uses streaming JSON parsing to handle large quote databases without running out of memory:

- **Pass 1**: Scans file to extract only quote IDs (~4 bytes each)
- **Pass 2**: Fetches full quote text only when needed for display

This allows hundreds of quotes without memory issues.

## License

MIT License — See [LICENSE](LICENSE) for details.

## Acknowledgments

- [GxEPD2](https://github.com/ZinggJM/GxEPD2) by Jean-Marc Zingg
- [ArduinoJson](https://arduinojson.org/) by Benoît Blanchon
- [LaskaKit](https://www.laskakit.cz/) for the ESPink board
