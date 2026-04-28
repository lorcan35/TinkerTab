# TinkerTab

**ESP32-P4 firmware for the M5Stack Tab5 -- the face of the TinkerClaw voice assistant platform.**

```
 +-----------+         WebSocket          +-------------+
 |  Tab5     | ---- voice/text/config --> |  Dragon Q6A |
 |  (Face)   | <-- STT/LLM/TTS/audio --- |  (Brain)    |
 +-----------+                            +-------------+
  ESP32-P4                                 RK3588 / NPU
  720x1280 MIPI DSI                        STT (Moonshine)
  4-mic TDM array                          LLM (Llama / cloud)
  ES8388 speaker                           TTS (Piper)
  LVGL v9 native UI                        Embeddings + memory
```

TinkerTab is a **thin client** -- it owns the hardware (display, touch, microphone, speaker,
camera, sensors) and the native LVGL UI, but all intelligence lives on the
[Dragon Q6A server (TinkerBox)](https://github.com/lorcan35/TinkerBox). Audio frames stream
over WebSocket; STT, LLM reasoning, and TTS synthesis happen server-side and stream back.

The result is a privacy-first, local-first voice assistant that runs entirely on your own
hardware. No cloud required (though cloud mode is available for higher-quality models via
OpenRouter).

---

## Table of Contents

- [Features](#features)
- [Hardware](#hardware)
- [Architecture](#architecture)
- [Prerequisites](#prerequisites)
- [Quick Start](#quick-start)
- [Build and Flash](#build-and-flash)
- [Configuration](#configuration)
- [Serial Commands](#serial-commands)
- [Debug Server](#debug-server)
- [Voice Protocol](#voice-protocol)
- [UI Screens](#ui-screens)
- [Project Structure](#project-structure)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [License](#license)

---

## Features

- **Voice Assistant (Ask mode)** -- Push-to-talk voice interaction with multi-turn
  conversation. Speak a question, get a spoken answer. STT, LLM, and TTS all run on Dragon.
- **Dictation Mode** -- Unlimited-length voice-to-text transcription. No LLM processing,
  just raw STT with post-processing (title + summary generation).
- **Notes with Voice Recording** -- Create text or voice notes. Voice notes are recorded
  locally and transcribed asynchronously via Dragon.
- **Multi-turn Conversation** -- Session persistence across reboots via NVS-stored session
  IDs. Dragon maintains conversation context.
- **Cloud Mode** -- Toggle between local models (Moonshine STT + Piper TTS) and cloud
  models (OpenRouter) for higher quality at the cost of latency.
- **Settings Persistence** -- WiFi credentials, Dragon host/port, volume, brightness, cloud
  mode, device identity, and session ID all persist in NVS flash.
- **7+ UI Screens** -- Home, Notes, Settings, WiFi, Chat, Files, Camera (plus Splash on boot
  and full-screen Voice overlay).
- **SD Card Storage** -- Local audio recordings, camera captures, and file browsing on
  microSD.
- **Debug Server** -- HTTP server on port 8080 for screenshots, device info, touch injection,
  screen navigation, crash logs, and a live remote control page.
- **mDNS Discovery** -- Automatic Dragon server discovery on the local network.
- **Hardware Diagnostics** -- Full serial command interface for testing every subsystem
  (audio, mic, camera, IMU, RTC, battery, touch, SD card, I2C scan).
- **Widget Platform (v1)** -- Skills on Dragon emit typed widget state (`live`, `card`,
  `list`, `chart`, `media`, `prompt`); Tab5 renders it opinionatedly. New features ship as
  Python files on Dragon, no firmware flash. See [`docs/WIDGETS.md`](./docs/WIDGETS.md).

---

## Hardware

TinkerTab targets the **M5Stack Tab5** development kit built around the **ESP32-P4** SoC.

| Component           | Part              | Interface         | Details                              |
|---------------------|-------------------|-------------------|--------------------------------------|
| SoC                 | ESP32-P4          | --                | Dual-core RISC-V, 400 MHz, 16 MB flash, PSRAM |
| Display             | ST7123            | MIPI DSI (2-lane) | 720x1280, RGB565/888, 70 MHz pixel clock |
| Touch               | GT911             | I2C               | Capacitive multi-touch               |
| WiFi Co-processor   | ESP32-C6          | SDIO (4-bit)      | ESP-Hosted, 802.11ax                 |
| Speaker DAC         | ES8388            | I2S STD (TX)      | 48 kHz, I2C addr 0x10               |
| Microphone ADC      | ES7210            | I2S TDM (RX)      | 4-channel, 48 kHz, I2C addr 0x40    |
| Camera              | SC202CS           | MIPI CSI (2-lane) | SCCB addr 0x36, 24 MHz XCLK         |
| SD Card             | --                | SDMMC Slot 0      | LDO channel 4                        |
| IMU                 | BMI270            | I2C               | 6-axis accel + gyro, addr 0x68       |
| RTC                 | RX8130CE          | I2C               | Real-time clock, addr 0x32, NTP sync |
| Battery Monitor     | INA226            | I2C               | Voltage, current, power, addr 0x40   |
| IO Expanders        | PI4IOE5V6416 x2   | I2C               | LCD/touch reset, WiFi power, USB 5V  |
| Backlight           | PWM               | GPIO 22           | 5 kHz, 0-100% brightness             |

### Pin Map (abridged)

```
I2C:    SDA=31, SCL=32 (400 kHz)
I2S:    MCLK=30, BCK=27, WS=29, DOUT=26, DIN=28
SDIO:   CLK=12, CMD=13, D0=11, D1=10, D2=9, D3=8, RST=15
SD:     CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42
Camera: XCLK=36
Touch:  INT=23
```

Full pin definitions are in [`main/config.h`](main/config.h).

---

## Architecture

### Thin Client Design

TinkerTab is deliberately a thin client. It handles hardware I/O and UI rendering; all
AI processing happens on Dragon:

```
Tab5 (TinkerTab)                     Dragon Q6A (TinkerBox)
+---------------------------+        +---------------------------+
| Hardware Layer             |        | Voice Pipeline            |
|  Display, Touch, Mic,     |        |  Moonshine STT            |
|  Speaker, Camera, SD,     |        |  Llama / Cloud LLM        |
|  IMU, RTC, Battery        |        |  Piper TTS                |
+---------------------------+        |  Qwen3-Embedding 0.6B     |
| Service Registry           |        +---------------------------+
|  Storage, Display, Audio,  |        | API Layer                 |
|  Network, Dragon           |        |  WebSocket /ws/voice      |
+---------------------------+        |  REST /api/handshake      |
| Mode Manager (FSM)         |        |  Health /health           |
|  IDLE -> STREAMING         |        +---------------------------+
|  IDLE -> VOICE             |        | Session + Memory          |
|  STREAMING -> VOICE        |        |  Conversation history     |
+---------------------------+        |  RAG / embeddings         |
| LVGL v9 UI                 |        +---------------------------+
|  Home, Notes, Settings,   |
|  WiFi, Chat, Files, Camera|
+---------------------------+
| Voice Client (WebSocket)   |
|  PCM 16kHz -> Dragon      |
|  TTS audio <- Dragon      |
+---------------------------+
```

### Mode Manager

The device operates in one mode at a time to prevent DMA RAM exhaustion from concurrent
SDIO usage:

| Mode        | Active Services                            | Description                          |
|-------------|--------------------------------------------|--------------------------------------|
| `IDLE`      | WiFi                                       | No streaming, no voice               |
| `STREAMING` | WiFi + MJPEG + Touch WS                   | Dragon desktop streaming (default)   |
| `VOICE`     | WiFi + Voice WS + Mic + Speaker           | Voice assistant (MJPEG paused)       |
| `BROWSING`  | WiFi + MJPEG + Touch WS                   | Reserved (same as STREAMING)         |

Mode switches are mutex-protected and idempotent. The mode manager stops services from the
old mode, waits for DMA to settle, then starts services for the new mode.

### Service Registry

All subsystems are managed through a service lifecycle:

```
NONE -> INITIALIZED -> RUNNING -> STOPPED -> RUNNING -> ...
                                     \-> ERROR (needs recovery)
```

Services: `STORAGE`, `DISPLAY`, `AUDIO`, `NETWORK`, `DRAGON`

### Boot Sequence

1. Platform init (I2C bus, IO expanders, WiFi power)
2. Service init (allocate, configure hardware for all services)
3. Peripheral drivers (camera, IMU, RTC, battery)
4. Service start: Storage -> Display -> Audio -> Network
5. Dragon link start (needs WiFi)
6. LVGL init and splash screen
7. Home screen transition with deferred overlay creation
8. Debug HTTP server start
9. NTP time sync (best-effort)
10. Serial command loop

---

## Prerequisites

| Requirement               | Version          | Notes                                       |
|---------------------------|------------------|---------------------------------------------|
| ESP-IDF                   | v5.5.2           | Must match `dependencies.lock`              |
| Python                    | 3.12+            | For esptool, serial monitor                 |
| M5Stack Tab5              | --               | ESP32-P4 hardware                           |
| Dragon Q6A server         | --               | Running [TinkerBox](https://github.com/lorcan35/TinkerBox) |
| USB-C cable               | --               | For flashing and serial console             |

### Install ESP-IDF

Follow the [ESP-IDF Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32p4/get-started/index.html)
or use the one-liner:

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32p4
```

---

## Quick Start

```bash
# 1. Clone the repository
git clone https://github.com/lorcan35/TinkerTab.git
cd TinkerTab

# 2. Activate ESP-IDF environment
. ~/esp/esp-idf/export.sh

# 3. Set target and build
idf.py set-target esp32p4
idf.py build

# 4. Flash to device
idf.py -p /dev/ttyACM0 flash

# 5. If the board enters ROM download mode after flashing, trigger a watchdog reset:
python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac

# 6. Monitor serial output
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=5)
time.sleep(0.3)
s.write(b'\r')
while True:
    if s.in_waiting:
        print(s.read(s.in_waiting).decode('utf-8', errors='replace'), end='', flush=True)
"
```

On first boot, Tab5 will:
1. Connect to the WiFi network configured in `sdkconfig.defaults`
2. Attempt mDNS discovery of the Dragon server
3. Display the TinkerOS home screen

---

## Build and Flash

### Environment Setup

```bash
# Source ESP-IDF (required in every terminal session)
. ~/esp/esp-idf/export.sh
```

### Full Build

```bash
# Set target (required after clean builds or target changes)
idf.py set-target esp32p4

# Build
idf.py build
```

### Flash

```bash
# Flash via USB (adjust port as needed)
idf.py -p /dev/ttyACM0 flash
```

### Clean Build

After `sdkconfig` changes, pulling new components, or target changes, always do a full
clean build:

```bash
idf.py fullclean build
```

### Partition Table

The 16 MB flash is partitioned as follows:

| Partition  | Type     | Offset     | Size       | Purpose                 |
|------------|----------|------------|------------|-------------------------|
| `nvs`      | data/nvs | 0x9000     | 24 KB      | Settings persistence    |
| `phy_init` | data/phy | 0xF000     | 4 KB       | WiFi PHY calibration    |
| `factory`  | app      | 0x10000    | ~4 MB      | Application firmware    |
| `coredump` | data     | 0x400000   | 64 KB      | Crash core dumps        |
| `storage`  | data/fat | 0x410000   | ~12 MB     | Internal FAT storage    |

---

## Configuration

### WiFi and Dragon Server

Default credentials are set in `sdkconfig.defaults`:

```
CONFIG_TAB5_WIFI_SSID="TinkerNet"
CONFIG_TAB5_WIFI_PASS="YourPassword"
CONFIG_TAB5_DRAGON_HOST="192.168.1.91"
CONFIG_TAB5_DRAGON_PORT=3502
```

You can also change these at runtime via:
- The **Settings** screen on the device
- The **WiFi** screen for network selection
- NVS persistence (settings survive reboots)

To change defaults via menuconfig:

```bash
idf.py menuconfig
# Navigate to: Component config -> TinkerTab Configuration
```

### NVS Settings

All runtime settings persist in NVS flash. The API is in [`main/settings.h`](main/settings.h):

| Setting          | Type     | Default        | Description                    |
|------------------|----------|----------------|--------------------------------|
| WiFi SSID        | string   | from sdkconfig | WiFi network name              |
| WiFi Password    | string   | from sdkconfig | WiFi password                  |
| Dragon Host      | string   | from sdkconfig | Dragon server IP/hostname      |
| Dragon Port      | uint16   | 3502           | Dragon server port             |
| Brightness       | uint8    | 80             | Display brightness (0-100%)    |
| Volume           | uint8    | 70             | Speaker volume (0-100%)        |
| Device ID        | string   | auto (MAC)     | Generated on first boot        |
| Hardware ID      | string   | auto (MAC)     | MAC address (AA:BB:CC:DD:EE:FF)|
| Session ID       | string   | (empty)        | Dragon conversation session    |
| Cloud Mode       | uint8    | 0              | 0=local, 1=cloud (OpenRouter)  |

### Key sdkconfig Options

| Option                              | Value    | Why                                         |
|-------------------------------------|----------|---------------------------------------------|
| `CONFIG_SPIRAM_SPEED_200M`          | y        | PSRAM at 200 MHz for DSI framebuffer DMA    |
| `CONFIG_CACHE_L2_CACHE_256KB`       | y        | 256 KB L2 cache for DSI video streaming     |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE`   | 16384    | Large main stack for all driver inits       |
| `CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS` | n  | Disabled -- P4 PSRAM XIP TLSP crash fix     |
| `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` | y      | Crash diagnostics saved to flash            |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` | 131072 | Reserve 128 KB internal RAM for DMA       |

---

## Serial Commands

Connect via serial (115200 baud) and type commands followed by Enter:

### System

| Command            | Description                                              |
|--------------------|----------------------------------------------------------|
| `info`             | Chip info, heap, peripheral status, current mode         |
| `heap`             | Free heap and PSRAM                                      |
| `mode`             | Current mode (IDLE/STREAMING/VOICE)                      |
| `services`         | Print service registry status table                      |
| `tasks`            | FreeRTOS task count, heap stats, reset reason            |
| `reboot`           | Software restart                                         |

### Network and Dragon

| Command            | Description                                              |
|--------------------|----------------------------------------------------------|
| `wifi`             | WiFi connection status and configured SSID               |
| `dragon`           | Dragon link state, target host:port, MJPEG FPS           |

### Display

| Command            | Description                                              |
|--------------------|----------------------------------------------------------|
| `red`              | Fill display red (RGB565 test)                           |
| `green`            | Fill display green                                       |
| `blue`             | Fill display blue                                        |
| `white`            | Fill display white                                       |
| `black`            | Fill display black                                       |
| `bright <0-100>`   | Set backlight brightness                                 |
| `pattern [0-3]`    | Display test pattern (bars, gradients)                   |

### Peripherals

| Command            | Description                                              |
|--------------------|----------------------------------------------------------|
| `scan`             | Scan I2C bus for devices                                 |
| `touch`            | Poll touch input for 5 seconds                           |
| `touchdiag`        | Touch diagnostics with verbose 5-second poll             |
| `sd`               | SD card mount status, total/free space                   |
| `cam`              | Camera info, capture frame, save JPEG to SD              |
| `audio`            | Audio codec status, play 440 Hz test tone (1 second)     |
| `mic`              | Mic status, record 1 second, report RMS level            |
| `micdiag`          | Detailed mic diagnostics                                 |
| `imu`              | Accelerometer, gyroscope readings, orientation           |
| `rtc`              | RTC date/time                                            |
| `ntp`              | Sync RTC from NTP server                                 |
| `bat`              | Battery voltage, current, power, percent, charge status  |

### Voice

| Command            | Description                                              |
|--------------------|----------------------------------------------------------|
| `voice`            | Spawn voice E2E test (connect, record 3s, get response)  |

### Notes

| Command            | Description                                              |
|--------------------|----------------------------------------------------------|
| `noteadd <text>`   | Add a text note                                          |
| `notes`            | List all notes                                           |
| `notedel <idx>`    | Delete note by slot index                                |
| `notetest`         | Add 3 test notes                                         |
| `noteclear`        | Delete all failed/empty recording notes                  |

---

## Debug Server

When WiFi is connected, an HTTP debug server runs on **port 8080**. Access it at
`http://<device-ip>:8080/`.

### Endpoints

| Method | Path               | Description                                          |
|--------|--------------------|------------------------------------------------------|
| GET    | `/`                | Interactive HTML remote control page                 |
| GET    | `/screenshot`      | Framebuffer capture as BMP (720x1280 RGB565)         |
| GET    | `/screenshot.bmp`  | Same as above (explicit extension)                   |
| GET    | `/info`            | Device info as JSON (heap, peripherals, mode, etc.)  |
| POST   | `/touch`           | Inject touch event (JSON body with x, y, action)     |
| POST   | `/reboot`          | Restart the device                                   |
| GET    | `/log`             | Heap and FreeRTOS task info                          |
| POST   | `/open`            | Navigate to a specific UI screen                     |
| GET    | `/crashlog`        | Read last core dump from flash                       |

### Examples

```bash
# Take a screenshot
curl -s -o screen.bmp http://192.168.70.128:8080/screenshot

# Get device info
curl -s http://192.168.70.128:8080/info | python3 -m json.tool

# Inject a tap at coordinates (360, 640)
curl -s -X POST http://192.168.70.128:8080/touch \
  -d '{"x":360,"y":640,"action":"tap"}'

# Reboot the device
curl -s -X POST http://192.168.70.128:8080/reboot

# Navigate to a screen
curl -s -X POST http://192.168.70.128:8080/open -d '{"screen":"settings"}'
```

---

## Voice Protocol

TinkerTab implements the client side of the TinkerClaw voice WebSocket protocol.
The server specification lives in TinkerBox at `docs/protocol.md`.

**Endpoint:** `ws://<dragon-host>:3502/ws/voice`

### Tab5 to Dragon

| Message Type                           | Format  | Description                          |
|----------------------------------------|---------|--------------------------------------|
| `{"type":"start"}`                     | JSON    | Begin voice capture session          |
| Binary PCM frames                      | binary  | 16-bit, 16 kHz, mono (20 ms chunks) |
| `{"type":"stop"}`                      | JSON    | End voice capture, trigger STT/LLM/TTS |
| `{"type":"cancel"}`                    | JSON    | Abort current processing             |
| `{"type":"ping"}`                      | JSON    | Keepalive heartbeat (every 15s)      |
| `{"type":"text","content":"..."}`      | JSON    | Text input (skips STT, goes to LLM) |
| `{"type":"register",...}`              | JSON    | Device registration on connect       |
| `{"type":"config_update","voice_mode":0\|1\|2\|3,"llm_model":"..."}` | JSON | Voice mode + LLM picker (modes: 0=Local 1=Hybrid 2=Cloud 3=TinkerClaw). Old `cloud_mode` boolean still accepted for backward compat |
| `{"type":"clear"}`                     | JSON    | Clear conversation context           |

### Dragon to Tab5

| Message Type                                      | Format  | Description              |
|---------------------------------------------------|---------|--------------------------|
| `{"type":"session_start","session_id":"..."}`     | JSON    | Session assigned/resumed |
| `{"type":"stt","text":"..."}`                     | JSON    | Speech-to-text result    |
| `{"type":"llm","text":"..."}`                     | JSON    | LLM response text (streams in) |
| `{"type":"tts_start"}`                            | JSON    | TTS audio about to begin |
| Binary TTS frames                                 | binary  | PCM 16-bit 16 kHz audio |
| `{"type":"tts_end"}`                              | JSON    | TTS audio complete       |
| `{"type":"error","message":"..."}`                | JSON    | Error from Dragon pipeline |
| `{"type":"dictation_summary",...}`                | JSON    | Post-processed dictation result |

### Audio Pipeline

```
Capture:  ES7210 (48 kHz, 4-ch TDM) -> extract slot 0 -> downsample 3:1 -> 16 kHz mono -> WebSocket
Playback: WebSocket -> 16 kHz PCM -> upsample 1:3 -> 48 kHz -> ES8388 DAC -> speaker
```

---

## UI Screens

The LVGL v9 UI uses a 4-page vertical tileview as the main navigation, with additional
screens accessible from within pages. Bottom navigation bar: Home | Notes | Voice | Settings.

### Home (Page 0)
Clock display, tappable voice orb, last note preview, quick action buttons.
Status bar with time, privacy badge, WiFi indicator, and battery level.

### Notes (Page 1)
Voice and text notes list. Supports voice recording with live waveform visualization,
background transcription via Dragon, and note management (add, delete, clear failed).

### Voice (Page 2)
Full voice overlay with animated orb. Push-to-talk interface for Ask mode and Dictation
mode. Shows STT transcript and LLM response text in real-time.

### Settings (Page 3)
Overlay-based device configuration with sectioned layout: Display (brightness), Network
(WiFi SSID/password, Dragon host/port), Voice (mode, LLM model), Storage, Battery, OTA,
and About. All changes persist to NVS immediately.

### WiFi
Network scanning and selection. Shows available SSIDs, signal strength, and connection
status.

### Chat
Text-based conversation interface. Sends text directly to Dragon LLM (skips STT).

### Files
SD card file browser with directory navigation and file info display.

### Camera
Live camera preview from SC202CS, JPEG capture, save to SD card.

### Splash
Boot splash screen with progress bar and status messages. Shown during hardware
initialization, replaced by Home screen when ready.

### Keyboard
On-screen keyboard overlay, available on any screen that needs text input.

---

## Project Structure

```
TinkerTab/
|-- main/
|   |-- main.c                 Boot sequence, serial command loop
|   |-- config.h               All pin definitions and constants
|   |-- voice.c / voice.h      Voice streaming (WS client, mic capture, TTS playback)
|   |-- audio.c / audio.h      ES8388 DAC + I2S TX/RX setup via esp_codec_dev
|   |-- mic.c                  ES7210 quad-mic capture via esp_codec_dev
|   |-- dragon_link.c / .h     Dragon mDNS discovery + connection state machine
|   |-- mdns_discovery.c / .h  mDNS service resolution
|   |-- mode_manager.c / .h    Mode FSM (IDLE/STREAMING/VOICE/BROWSING)
|   |-- service_registry.c / .h  Service lifecycle management
|   |-- service_audio.c        Audio service (init/start/stop)
|   |-- service_display.c      Display service
|   |-- service_network.c      Network (WiFi) service
|   |-- service_storage.c      Storage (SD + NVS) service
|   |-- service_dragon.c       Dragon link service
|   |-- settings.c / .h        NVS-backed persistent settings
|   |-- display.c / .h         MIPI DSI ST7123 display driver
|   |-- esp_lcd_st7123.c / .h  ST7123 LCD panel driver
|   |-- touch.c / .h           GT911 touch controller
|   |-- wifi.c / .h            ESP32-C6 WiFi via ESP-Hosted SDIO
|   |-- sdcard.c / .h          SD card (SDMMC) mount and file ops
|   |-- camera.c / .h          SC202CS camera (MIPI CSI)
|   |-- imu.c / .h             BMI270 IMU (accel + gyro + orientation)
|   |-- rtc.c / .h             RX8130CE real-time clock + NTP sync
|   |-- battery.c / .h         INA226 battery monitor
|   |-- bluetooth.c / .h       BLE stub (waiting for ESP-Hosted support)
|   |-- io_expander.c / .h     PI4IOE5V6416 GPIO expanders
|   |-- debug_server.c / .h    HTTP debug server (port 8080)
|   |-- mjpeg_stream.c / .h    MJPEG video stream receiver
|   |-- udp_stream.c / .h      UDP video stream receiver
|   |-- touch_ws.c / .h        Touch event WebSocket forwarder
|   |-- ui_core.c / .h         LVGL init, lock/unlock, tick
|   |-- ui_splash.c / .h       Boot splash screen
|   |-- ui_home.c / .h         Home screen (4-page tileview)
|   |-- ui_voice.c / .h        Voice overlay (orb, PTT, waveform)
|   |-- ui_notes.c / .h        Notes screen (text + voice notes)
|   |-- ui_chat.c / .h         Chat/conversation screen
|   |-- ui_settings.c / .h     Settings screen
|   |-- ui_wifi.c / .h         WiFi network scanner/selector
|   |-- ui_files.c / .h        SD card file browser
|   |-- ui_camera.c / .h       Camera preview and capture
|   |-- ui_audio.c / .h        Audio test/diagnostic UI
|   |-- ui_keyboard.c / .h     On-screen keyboard overlay
|   |-- ui_port.h              LVGL display/input port config
|   |-- lv_conf.h              LVGL build configuration
|   |-- Kconfig.projbuild      Menuconfig options (WiFi, Dragon)
|   |-- idf_component.yml      Component dependencies
|   +-- CMakeLists.txt          Build file list
|
|-- components/                 Local ESP-IDF components
|-- managed_components/         Auto-downloaded dependencies (lvgl, esp_hosted, etc.)
|-- sim/                        Desktop SDL2 simulator (WIP)
|-- tests/e2e/                  Python e2e harness (driver.py + scenario runner, PR #295)
|-- docs/                       Active reference docs (VOICE_PIPELINE, WIDGETS, ...)
|   +-- historical/             Archived superseded docs (BUILD_PLAN, STREAMING, etc.)
|-- bin/                        Utility scripts
|
|-- CMakeLists.txt              Top-level CMake project file
|-- sdkconfig.defaults          Default build configuration
|-- sdkconfig.defaults.esp32p4  P4-specific overrides
|-- sdkconfig.defaults.linux    Linux simulator overrides
|-- partitions.csv              Flash partition table (16 MB)
|-- dependencies.lock           Locked component versions
|-- CLAUDE.md                   Developer instructions and institutional knowledge
|-- LEARNINGS.md                30+ entries of hard-won debugging knowledge
+-- LICENSE                     MIT License
```

---

## Troubleshooting

### DMA Memory Exhaustion

**Symptom:** Crash or allocation failure when switching between MJPEG streaming and voice.

**Cause:** SDIO WiFi, I2S audio, and MIPI DSI all compete for limited internal DMA RAM
(~512 KB shared with FreeRTOS).

**Fix:** The mode manager ensures only one heavy subsystem runs at a time. If you hit
allocation failures, check that `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` is set to at least
131072 in `sdkconfig.defaults`.

**Related (audit #80 / wave 6, 2026-04-20):** If Tab5 stays up (LVGL running, `/info`
returns `wifi_connected=True`) but the router shows ARP incomplete and every Dragon WS
reconnect fails with `ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT errno=119`, the DMA pool has
dropped below ~15 KB free and the WiFi driver can't allocate its Rx ring slot. The heap
watchdog in `main/heap_watchdog.c` now catches this: when `MALLOC_CAP_DMA |
MALLOC_CAP_INTERNAL` free drops below 16 KB for 2 consecutive 60 s samples, it calls
`esp_restart()` (honoring a voice-active grace window). Before the fix the device would
stay radio-silent until the user serial-reflashed. This is a RECOVERY fix — deeper DMA
leak root-cause is tracked separately at issue #80.

### WiFi SDIO Init Failure

**Symptom:** WiFi never connects, ESP-Hosted errors in log.

**Cause:** Tab5 uses non-default SDIO pins and active-LOW reset for the C6 co-processor.

**Fix:** Ensure these are set in `sdkconfig.defaults`:
```
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=15
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_LOW=y
CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET="esp32c6"
```
Also verify that WiFi power is enabled via IO expander before SDIO init.

### LVGL Mutex / Background Task Crash

**Symptom:** Guru Meditation or abort when a background task accesses LVGL objects.

**Cause:** LVGL is not thread-safe. Background tasks must not touch LVGL objects directly.

**Fix:** Always bracket LVGL calls with `tab5_ui_lock()` / `tab5_ui_unlock()`. For screen
transitions, use `volatile bool s_destroying` guards to prevent access to objects that are
being deleted.

### P4 TLSP Crash (vTaskDelete)

**Symptom:** Crash in FreeRTOS task cleanup after `vTaskDelete(NULL)`.

**Cause:** ESP32-P4 with PSRAM XIP maps `.text` at 0x4800xxxx which
`esp_ptr_executable()` does not recognize, causing TLSP deletion callback to fault.

**Fix:** Use `vTaskSuspend(NULL)` instead of `vTaskDelete(NULL)` for self-deleting tasks.
`CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS` is disabled in sdkconfig. See issue #20.

### Board Enters ROM Download Mode After Flash

**Symptom:** Device does not boot after `idf.py flash`, serial shows ROM messages.

**Fix:** Trigger a watchdog reset to boot the flashed application:
```bash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
  --before no_reset --after watchdog_reset read_mac
```

### sdkconfig Changes Not Taking Effect

**Symptom:** Changed `sdkconfig.defaults` but behavior unchanged after build.

**Cause:** Incremental builds cache stale configuration.

**Fix:** Always run `idf.py fullclean build` after sdkconfig changes.

### Audio Playback Issues (ES8388)

**Symptom:** No sound, distorted audio, or `[MUSIC PLAYING]` log spam.

**Cause:** Custom ES8388 register writes conflict with `esp_codec_dev` library defaults.
I2S TX/RX BCLK mismatch when TX uses STD (2-slot) and RX uses TDM (4-slot) on the same bus.

**Fix:** Never write custom ES8388 registers. Use `es8388_codec_new()` from
`esp_codec_dev`. Both TX and RX must use TDM 4-slot mode for consistent BCLK (3.072 MHz).
See issue #46.

### WebSocket String Literal Crash

**Symptom:** Crash in `esp_transport_ws_send_raw()`.

**Cause:** ESP-IDF WS transport masks frames in-place. Passing a string literal (read-only
memory) causes a write fault.

**Fix:** Always copy the message to a mutable `malloc`/stack buffer before sending.

### Core Dump Analysis

If the device crashes, the core dump is saved to flash. Retrieve and analyze it:

```bash
# Read core dump
python -m esp_coredump -p /dev/ttyACM0 info_corefile

# Or via debug server (if device rebooted successfully)
curl -s http://<device-ip>:8080/crashlog
```

---

## Contributing

1. **Issue first** -- Create a GitHub issue before starting work: `gh issue create`
2. **Branch** -- Create a feature or fix branch from `main`
3. **Commit with issue reference** -- Every commit must reference an issue:
   - `fix: description (closes #N)` for bug fixes
   - `feat: description (refs #N)` for features
4. **Keep commits atomic** -- One fix per commit, push after each
5. **Push and merge** -- Push to origin, merge to main

### Issue Format

```
## Bug / Problem
What the user sees. Symptoms, frequency, impact.

## Root Cause
Technical explanation of WHY this happens.

## Culprit
Exact file(s) and line(s) responsible.

## Fix
What was changed and why this fixes it.

## Resolved
Commit hash + PR if applicable.
```

### Code Style

- C11, no C++ (the firmware will stay in C)
- Follow `.clang-format` for formatting
- Use `ESP_LOGI/W/E` for logging, `printf` for serial command output
- Large buffers (>4 KB) go in PSRAM via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`
- All FreeRTOS tasks doing network + LVGL callbacks need minimum 8 KB stack
- Read `LEARNINGS.md` before making changes -- it contains 30+ hard-won debugging entries

---

## License

MIT License. Copyright (c) 2026 TinkerClaw.

See [LICENSE](LICENSE) for the full text.
