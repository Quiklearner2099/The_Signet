# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**The Signet** is an Arduino firmware project for a Morse code beacon device that transmits messages via IR (invisible) or RGB (visible) LED. It's designed for protest messaging and video steganography, embedding Morse-encoded messages into recorded video footage using blinking light sources.

Hardware target: **Seeed Studios XIAO ESP32-C6** (though code can be adapted to other ESP32 boards)

## Building and Uploading

This is an Arduino sketch project. The main firmware file is `The_Signet.ino`.

### Arduino IDE
1. Open `The_Signet.ino` in Arduino IDE
2. Select board: **Tools > Board > ESP32 Arduino > XIAO_ESP32C6**
3. Select port: **Tools > Port > [Your COM port]**
4. Click **Upload** or press Ctrl+U

### Arduino CLI
```bash
# Compile
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6 The_Signet.ino

# Upload
arduino-cli upload -p [PORT] --fqbn esp32:esp32:XIAO_ESP32C6 The_Signet.ino

# Compile and upload
arduino-cli compile --upload -p [PORT] --fqbn esp32:esp32:XIAO_ESP32C6 The_Signet.ino
```

### Required Libraries
Install via Arduino Library Manager or CLI:
- **WiFi** (built-in with ESP32 core)
- **WebServer** (built-in with ESP32 core)
- **DNSServer** (built-in with ESP32 core)
- **FastLED** (for WS2812B RGB LED control)
- **ArduinoJson** (for JSON parsing in web API)
- **LittleFS** (built-in with ESP32 core, for filesystem)

### Filesystem Upload
The device serves a splash image (`/bb.jpg`) from LittleFS. To upload filesystem data:

**Arduino IDE:**
1. Install ESP32 LittleFS uploader plugin
2. Place `bb.jpg` in the `data/` folder
3. Tools > ESP32 Sketch Data Upload

**Arduino CLI / esptool:**
```bash
# Generate LittleFS image
mklittlefs -c data -s 0x100000 littlefs.bin

# Upload to ESP32 (adjust offset for your partition table)
esptool.py --port [PORT] write_flash 0x290000 littlefs.bin
```

## Architecture Overview

### Core Components

**1. WiFi Access Point + Captive Portal**
- Device creates WiFi AP on boot: `The_Signet_XXXXXX` (last 3 MAC bytes appended)
- No password, open network
- Captive portal redirects all DNS to device IP (192.168.4.1)
- Web UI served from `INDEX_HTML` embedded in flash
- **Auto-shutdown**: AP turns off after 90 seconds of UI inactivity to save power

**2. Dual LED Output System**
- **RGB LED (WS2812B)**: Controlled via FastLED library on PIN_RGB (D5)
- **IR LED (850nm)**: Controlled via software PWM on PIN_IR (D8)
- Both LEDs are mutually exclusive - only one active at a time based on mode

**3. FreeRTOS Task Architecture**
The firmware uses FreeRTOS tasks (ESP32's native OS):
- `irPwmTask`: Software PWM generation for IR LED (200Hz, priority 2)
- `morseTask`: Morse code playback loop (priority 1)
- Main loop: Handles WiFi/web server and deep sleep switch monitoring

**4. Morse Code Engine**
- International Morse Code (ITU standard) lookup table in `MORSE[]` array
- Timing based on standard units (DOT=160ms, DASH=3×DOT, etc.)
- Supports A-Z, 0-9, and common punctuation
- Thread-safe text access via mutex (`textMutex`)

**5. Deep Sleep Management**
- Hardware switch on PIN_SLEEP_SW (D2) triggers deep sleep
- Wake-up via EXT1 wakeup on GPIO2 going LOW
- All LEDs and WiFi shutdown before sleep entry

### Pin Configuration (XIAO ESP32-C6)
```
PIN_RGB      D5 (GPIO5)  - WS2812B RGB LED data
PIN_IR       D8 (GPIO8)  - IR LED PWM drive
PIN_SLEEP_SW D2 (GPIO2)  - Sleep switch (SPST to GND, INPUT_PULLUP)
```

### State Management

Global `AppState` structure holds all runtime configuration:
- `mode`: DISCREET (IR) or VISIBLE (RGB)
- `color`: C_RED, C_GREEN, or C_BLUE (for visible mode)
- `intensity`: I_LOW, I_MED, or I_HIGH
- `dazzle`: Boolean flag for RGB flash effect between message loops
- `text`: Message string (mutex-protected)
- `playing`: Playback state flag

Access `state.text` only through thread-safe helpers:
- `getTextCopy()`: Read text safely
- `setTextSafe(newText)`: Write text safely

### Web API Endpoints

All endpoints operate on the WiFi AP (192.168.4.1):

- `GET /` - Serve web UI
- `GET /api/state` - Get current device state (JSON)
- `POST /api/update` - Update settings (mode, intensity, color, dazzle)
- `POST /api/play` - Start playback with message text
- `POST /api/stop` - Stop playback
- `GET /bb.jpg` - Serve splash image from LittleFS

### Precompiled Binaries

The `Precompiled/XIAO ESP32-C6/` folder contains ready-to-flash binaries:
- `The Signet v1.0.0.bin` - Main firmware binary
- `CHECKSUMS.md` - SHA256 checksums for verification

Flash with esptool:
```bash
esptool.py --port [PORT] write_flash 0x0 "The Signet v1.0.0.bin"
```

## Development Guidelines

### Adding Morse Characters
To add new characters to the Morse lookup table:
1. Add entry to `MORSE[]` array (line 238)
2. Update `MORSE_LEN` calculation (automatic via sizeof)
3. Characters are case-insensitive (auto-converted to uppercase)

### Modifying Timing
Morse timing constants (line 234):
- `UNIT`: Base timing unit (160ms)
- `DOT`, `DASH`: Symbol durations
- `GAP_INTRA`, `GAP_LETTER`, `GAP_WORD`: Spacing between elements
- `DISCREET_LOOP_GAP`: Delay between message repeats in IR mode
- Dazzle effect timing: `VISIBLE_DAZZLE_OFF1`, `VISIBLE_DAZZLE_OFF2`, `VISIBLE_DAZZLE_FLASH`

### Adjusting LED Brightness
Modify intensity mappings in:
- `rgbBrightnessFor()` (line 206): RGB brightness levels (0-255)
- `irDutyFor()` (line 215): IR PWM duty cycle (0-255)

### Changing AP Settings
- Modify `AP_SSID_BASE` (line 108) to change SSID prefix
- Adjust `AP_IDLE_TIMEOUT_MS` (line 132) for different auto-shutdown delay
- SSID automatically appends last 3 bytes of MAC address for uniqueness

### Port to Different ESP32 Boards
1. Update pin definitions (lines 98-105) to match your board
2. Adjust FQBN in compile commands
3. Verify GPIO capabilities (RGB requires any GPIO, IR needs PWM-capable, Sleep switch needs RTC GPIO for wakeup)
4. Update board-specific comments in header (lines 46-51)

## Key Technical Details

**Thread Safety**: The `state.text` field is protected by a mutex because it's accessed from both the web server (main loop) and the `morseTask`. Always use `getTextCopy()` and `setTextSafe()` rather than direct access.

**Software PWM Rationale**: IR LED uses software PWM instead of hardware LEDC because hardware PWM channels may conflict with FastLED's RMT usage on some ESP32 variants. This is noted as potentially changing to hardware PWM in future releases (line 146).

**Captive Portal Logic**: The `captivePortal()` function redirects any non-IP hostname to the device IP, ensuring that devices connecting to the AP are automatically directed to the UI regardless of what URL they try to visit.

**Stateless by Power Design**: No persistent storage of messages or settings. Device resets to defaults on power cycle, ensuring no message history is retained (privacy/security feature).

**Web UI**: Single-page app embedded as a C++ raw string literal (`INDEX_HTML`, lines 356-542). Uses vanilla JavaScript (no frameworks) for minimal footprint. Splash screen stored in LittleFS and shown once per browser session.

## Project Structure

```
The_Signet/
├── The_Signet.ino          # Main firmware (single-file Arduino sketch)
├── data/                   # LittleFS filesystem content
│   └── bb.jpg             # Splash screen image
├── Precompiled/           # Pre-built binaries
│   └── XIAO ESP32-C6/
├── Schematic/             # Circuit diagrams (PDF)
├── PCB/                   # Gerber files and images
├── Housing/               # 3D printable STL files
└── Pictures/              # Product photos and diagrams
```

## Ethical Use Notice

This device is intended for lawful, non-violent freedom of expression, documentation, and journalistic messaging. It is designed as a tool to support free speech in environments with digital censorship. See README.md for complete ethical use guidelines.

## License

- **Software**: MIT License (see LICENSE - SOFTWARE)
- **Hardware**: CERN-OHL-P-2.0 (see LICENSE - HARDWARE)
