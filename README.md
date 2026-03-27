# TinkerTab

Standalone AI device firmware for the **M5Stack Tab5** (ESP32-P4).

TinkerTab turns the Tab5 into a full standalone device with a native LVGL UI, camera, audio, voice input, IMU, battery management, and optional Dragon mode for browser streaming.

## Features

- **Native LVGL UI** — home launcher, settings, camera, file browser, audio player, all sized for 720x1280 DPI
- **Voice AI Pipeline** — speak into Tab5, Dragon processes STT→LLM→TTS, response plays back on speaker (Whisper.cpp + Ollama + Piper)
- **Dragon Mode** — live MJPEG browser streaming + touch forwarding from Dragon Q6A
- **mDNS Auto-Discovery** — Tab5 finds Dragon via `_tinkerclaw._tcp` service, no hardcoded IPs
- **WiFi Config Screen** — scan networks, tap to connect, on-screen keyboard for password entry
- **NVS Settings Persistence** — WiFi credentials, Dragon host, brightness, volume survive power cycles
- **Debug Server** — HTTP dashboard on port 8080 with remote screenshots, touch injection, device info
- **Full Hardware Support** — all Tab5 peripherals: display, touch, WiFi, camera, audio, mic, IMU, RTC, battery, SD card

## Hardware

| Component | IC | Interface | Status |
|-----------|-----|-----------|--------|
| SoC | ESP32-P4 (RISC-V 2x400MHz, 32MB PSRAM, 16MB flash) | — | ✅ |
| WiFi/BLE | ESP32-C6-MINI-1U | SDIO 4-bit (ESP-Hosted) | ✅ |
| Display | ST7123 TDDI 720x1280 IPS | MIPI-DSI 2-lane 965Mbps | ✅ |
| Touch | ST7123 integrated 10-point | I2C 0x55 | ✅ |
| IO Exp. 1 | PI4IOE5V6416 | I2C 0x43 | ✅ |
| IO Exp. 2 | PI4IOE5V6416 | I2C 0x44 | ✅ |
| Backlight | LEDC PWM GPIO 22 | 5kHz | ✅ |
| HW JPEG | ESP32-P4 built-in | — | ✅ |
| SD Card | — | SDMMC 4-bit | ✅ |
| Camera | SC202CS (SC2356) 2MP | MIPI-CSI 2-lane | ✅ |
| Audio Codec | ES8388 | I2S + I2C 0x10 | ✅ |
| Mic ADC | ES7210 (dual, AEC) | I2S + I2C 0x40 | ✅ |
| Speaker | NS4150B 1W | I2S DAC + GPIO enable | ✅ |
| IMU | BMI270 6-axis | I2C 0x68 | ✅ |
| RTC | RX8130CE | I2C 0x32 | ✅ |
| Bat Monitor | INA226 | I2C 0x40 | ✅ |
| Charger | IP2326 | IO expander | ✅ |
| Battery | NP-F550 7.4V 2000mAh | Removable | — |

## Architecture

TinkerTab operates in two modes:

### Standalone Mode (LVGL)
Native UI running directly on ESP32-P4. No external dependencies.
- Clock, weather, camera viewfinder
- Voice recorder, music player
- Settings, file browser
- WiFi configuration with network scanning
- Basic AI via WiFi → OpenRouter API

### Dragon Mode (MJPEG)
When connected to a Dragon Q6A, Tab5 becomes a browser remote.
- Live browser streaming via CDP screencast
- Touch forwarding via WebSocket
- Full TinkerTab OS (React web app on Dragon)
- PingApp ecosystem, local Ollama AI

Auto-switches: boots standalone, discovers Dragon via mDNS, upgrades to Dragon mode when available.

## Prerequisites

```bash
# ESP-IDF v5.4.3 (v5.5.x has DSI bug — do NOT use)
# v5.4.3 required for PSRAM XIP + TCM stack fixes needed by ESP-Hosted
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.4.3
cd esp-idf && ./install.sh esp32p4
source export.sh
```

## Build & Flash

```bash
source ~/esp/esp-idf/export.sh
cd TinkerTab

# First time only
idf.py set-target esp32p4

# Build
idf.py build

# Flash (Tab5 connected via USB-C)
python -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after no_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/tinkertab.bin

# Monitor
idf.py -p /dev/ttyACM0 monitor
```

## Debug Server

TinkerTab runs an HTTP debug server on **port 8080** once WiFi is connected. Useful for remote development.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | HTML dashboard with live device info |
| `/screenshot` | GET | Returns current framebuffer as JPEG |
| `/info` | GET | JSON: heap, PSRAM, WiFi, battery, uptime |
| `/touch` | POST | Inject touch event (`?x=360&y=640`) |
| `/reboot` | POST | Restart the device |
| `/log` | GET | Recent log output |

**Grab a screenshot:**
```bash
# Find the Tab5 on your network (mDNS name: tinkertab.local)
curl http://tinkertab.local:8080/screenshot -o screen.jpg

# Or by IP
curl http://192.168.1.X:8080/screenshot -o screen.jpg
```

**Inject a touch:**
```bash
curl -X POST "http://tinkertab.local:8080/touch?x=360&y=640"
```

## Dragon Setup

The Dragon Q6A runs three systemd services for TinkerTab integration:

| Service | Port | Purpose |
|---------|------|---------|
| `tinkerclaw-stream` | 3501 | CDP browser screencast → MJPEG stream |
| `tinkerclaw-voice` | 3502 | Voice pipeline (STT/LLM/TTS) WebSocket |
| `tinkerclaw-mdns` | — | Publishes `_tinkerclaw._tcp` for auto-discovery |

```bash
# Enable and start all services
sudo systemctl enable --now tinkerclaw-stream tinkerclaw-voice tinkerclaw-mdns

# Check status
sudo systemctl status tinkerclaw-stream tinkerclaw-voice tinkerclaw-mdns
```

The Tab5 discovers Dragon automatically via mDNS — no hardcoded IP needed. The `_tinkerclaw._tcp` service record advertises the Dragon's IP and ports.

## Voice Pipeline

Full conversational AI: user speaks into Tab5 mic → Dragon processes STT → LLM → TTS → spoken response plays on Tab5 speaker.

**Stack:**
- **STT:** Whisper.cpp (runs on Dragon CPU/GPU)
- **LLM:** Ollama with local models (e.g. llama3, mistral)
- **TTS:** Piper (fast local neural TTS)

**Protocol:** Tab5 streams PCM 16kHz mono over WebSocket to Dragon port 3502. Dragon returns TTS audio as PCM frames.

**Quick start:**
```bash
cd dragon_voice
pip install -r requirements.txt
pip install pywhispercpp piper-tts
python -m dragon_voice   # starts ws://0.0.0.0:3502
```

See [docs/VOICE_PIPELINE.md](docs/VOICE_PIPELINE.md) for the full architecture, protocol spec, and latency analysis.

## NVS Settings

Settings are persisted to NVS (non-volatile storage) and survive power cycles:

- **WiFi SSID & password** — set via the WiFi config screen or serial
- **Dragon host** — auto-populated from mDNS discovery, or set manually
- **Brightness** — backlight level (0-100)
- **Volume** — speaker volume (0-100)

Settings are loaded at boot and applied before the UI starts. The WiFi config screen (`Settings → WiFi`) scans for networks, lets you tap to select, and enter passwords with the on-screen keyboard.

## Serial Commands

| Command | Description |
|---------|-------------|
| `info` | Chip info, heap, PSRAM, all peripheral status |
| `heap` | Free heap + PSRAM |
| `wifi` | WiFi status, Dragon connection |
| `stream` | MJPEG FPS counter |
| `touch` | 5-second touch test |
| `touchdiag` | Raw register diagnostics |
| `scan` | I2C bus scan |
| `red/green/blue/white/black` | Fill screen |
| `bright <0-100>` | Backlight brightness |
| `pattern [0-3]` | Test patterns |
| `sd` | SD card info (total/free space) |
| `cam` | Capture frame, save to SD |
| `audio` | Audio codec info + 440Hz test tone |
| `mic` | Record 1s, show RMS level |
| `imu` | Accelerometer + gyroscope + orientation |
| `rtc` | Current RTC date/time |
| `ntp` | Sync RTC from NTP server |
| `bat` | Battery voltage, current, power, % |
| `voice_start` | Start voice capture + pipeline |
| `voice_stop` | Stop voice capture |
| `reboot` | Restart device |

## Project Structure

```
TinkerTab/
├── CMakeLists.txt          # ESP-IDF project
├── sdkconfig.defaults      # Default config (ESP32-P4 + ESP-Hosted)
├── BUILD_PLAN.md           # Phased build plan with status
├── TINKERTAB_OS_SPEC.md    # Full UI/UX design specification
├── dragon_server.py        # Dragon-side CDP streaming server
├── dragon_voice/           # Voice pipeline server (STT/LLM/TTS)
│   ├── config.yaml         # Voice server configuration
│   ├── config.py           # Config loader with env overrides
│   ├── requirements.txt    # Python dependencies
│   ├── stt/                # Speech-to-text backends
│   ├── tts/                # Text-to-speech backends
│   └── llm/                # LLM backends
├── docs/
│   ├── stt-tts-research.md # Voice pipeline research notes
│   └── VOICE_PIPELINE.md   # Voice pipeline documentation
├── main/
│   ├── CMakeLists.txt      # Component build config
│   ├── idf_component.yml   # Managed dependencies
│   ├── Kconfig.projbuild   # menuconfig entries
│   ├── config.h            # Pin definitions & constants
│   ├── main.c              # Boot sequence, command loop
│   ├── display.c/h         # MIPI DSI ST7123 + HW JPEG
│   ├── esp_lcd_st7123.c/h  # ST7123 panel driver
│   ├── touch.c/h           # ST7123 TDDI touch
│   ├── io_expander.c/h     # PI4IOE5V6416 driver
│   ├── wifi.c/h            # ESP-Hosted WiFi STA
│   ├── settings.c/h        # NVS settings persistence
│   ├── mdns_discovery.c/h  # mDNS Dragon auto-discovery
│   ├── debug_server.c/h    # HTTP debug server (port 8080)
│   ├── mjpeg_stream.c/h    # MJPEG client (Dragon mode)
│   ├── touch_ws.c/h        # WebSocket touch forwarding
│   ├── sdcard.c/h          # SDMMC 4-bit SD card driver
│   ├── camera.c/h          # SC202CS MIPI-CSI camera
│   ├── audio.c/h           # ES8388 codec + NS4150B speaker
│   ├── mic.c               # ES7210 dual microphone
│   ├── imu.c/h             # BMI270 6-axis IMU
│   ├── rtc.c/h             # RX8130CE real-time clock
│   ├── battery.c/h         # INA226 battery monitor
│   ├── bluetooth.c/h       # BLE stub (ESP-Hosted pending)
│   ├── lv_conf.h           # LVGL v9 configuration
│   ├── ui_core.c/h         # LVGL display/touch/task integration
│   ├── ui_splash.c/h       # Boot splash screen
│   ├── ui_home.c/h         # Home screen launcher
│   ├── ui_settings.c/h     # Settings screen
│   ├── ui_wifi.c/h         # WiFi config screen
│   ├── ui_camera.c/h       # Camera viewfinder
│   ├── ui_files.c/h        # File browser
│   ├── ui_audio.c/h        # Audio player
│   ├── ui_keyboard.c/h     # On-screen keyboard
│   └── ui_voice.c/h        # Voice UI overlay
```

## Pin Mapping

### I2C Bus (System)
| Signal | GPIO |
|--------|------|
| SDA | 31 |
| SCL | 32 |

### SDIO (WiFi — P4 ↔ C6)
| Signal | GPIO |
|--------|------|
| CLK | 12 |
| CMD | 13 |
| D0 | 11 |
| D1 | 10 |
| D2 | 9 |
| D3 | 8 |
| RST | 15 (active low) |

### SD Card (SDMMC)
| Signal | GPIO |
|--------|------|
| CLK | 43 |
| CMD | 44 |
| D0 | 39 |
| D1 | 40 |
| D2 | 41 |
| D3 | 42 |

### Display (MIPI-DSI)
| Parameter | Value |
|-----------|-------|
| Lanes | 2 |
| Bitrate | 965 Mbps |
| DPI Clock | 70 MHz |
| Resolution | 720x1280 |
| Backlight | GPIO 22 (LEDC) |

### I2C Device Addresses
| Device | Address |
|--------|---------|
| IO Exp 1 (PI4IOE5V6416) | 0x43 |
| IO Exp 2 (PI4IOE5V6416) | 0x44 |
| Touch (ST7123) | 0x55 |
| Audio Codec (ES8388) | 0x10 |
| Audio ADC (ES7210) | 0x40 |
| IMU (BMI270) | 0x68 |
| RTC (RX8130CE) | 0x32 |
| Bat Monitor (INA226) | 0x40/0x41 |

## Known Issues

- **MJPEG DMA decode** — HW JPEG decoder + DPI framebuffer in PSRAM needs `esp_cache_msync()` after every frame or DMA sees stale data. Currently working but adds ~2ms per frame.
- **ESP-IDF v5.5.x** breaks DSI display (stripes/empty). Use v5.4.3.
- **ESP-IDF v5.4.2** missing PSRAM XIP + TCM stack fixes needed for ESP-Hosted. Use v5.4.3.
- **Camera is SC202CS** (SC2356), NOT SC2336 as M5Stack docs claim. SCCB address 0x36, chip ID 0xEB52.
- **SD Card slot**: Uses SDMMC SLOT_0 (IOMUX pins 39-44), not default SLOT_1. Needs LDO channel 4.
- **ESP-Hosted SDIO**: Slave target must be ESP32C6, reset is active LOW. Default Kconfig targets P4 EV Board pins.
- **Double boot**: Normal on first flash — PSRAM timing calibration triggers reset.
- **BLE**: ESP-Hosted doesn't forward BLE yet — stub driver returns `ESP_ERR_NOT_SUPPORTED`.

## Resources

- [M5Stack Tab5 Docs](https://docs.m5stack.com/en/core/Tab5)
- [Factory Firmware Guide](https://docs.m5stack.com/en/esp_idf/m5tab5/userdemo)
- [ESPP Tab5 Class](https://esp-cpp.github.io/espp/m5stack_tab5.html)
- [ESPHome Tab5](https://devices.esphome.io/devices/m5stack-tab5/)
- [CNX Review Part 1](https://www.cnx-software.com/2025/05/14/m5stack-tab5-review-part-1-unboxing-teardown-and-first-try-of-the-esp32-p4-and-esp32-c6-5-inch-iot-devkit/)
- [CNX Review Part 2](https://www.cnx-software.com/2025/05/18/m5stack-tab5-review-getting-started-esp32-p4-esp-idf-framework-arduino-ide/)

## License

MIT
