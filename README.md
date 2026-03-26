# TinkerTab

Standalone AI device firmware for the **M5Stack Tab5** (ESP32-P4).

TinkerTab turns the Tab5 into a full standalone device with a native LVGL UI, camera, audio, voice input, IMU, battery management, and optional Dragon mode for browser streaming.

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
| Camera | SC2336 2MP | MIPI-CSI 2-lane | ✅ |
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
- BLE smart home control
- Basic AI via WiFi → OpenRouter API

### Dragon Mode (MJPEG)
When connected to a Dragon Q6A, Tab5 becomes a browser remote.
- Live browser streaming via CDP screencast
- Touch forwarding via WebSocket
- Full TinkerTab OS (React web app on Dragon)
- PingApp ecosystem, local Ollama AI

Auto-switches: boots standalone, upgrades to Dragon mode when available.

## Prerequisites

```bash
# ESP-IDF v5.4.2 (v5.5.x has DSI bug — do NOT use)
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.4.2
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
| `reboot` | Restart device |

## Project Structure

```
TinkerTab/
├── CMakeLists.txt          # ESP-IDF project
├── sdkconfig.defaults      # Default config (ESP32-P4 + ESP-Hosted)
├── BUILD_PLAN.md           # Phased build plan with status
├── TINKERTAB_OS_SPEC.md    # Full UI/UX design specification
├── dragon_server.py        # Dragon-side CDP streaming server
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
│   ├── mjpeg_stream.c/h    # MJPEG client (Dragon mode)
│   ├── touch_ws.c/h        # WebSocket touch forwarding
│   ├── sdcard.c/h          # SDMMC 4-bit SD card driver
│   ├── camera.c/h          # SC2336 MIPI-CSI camera
│   ├── audio.c/h           # ES8388 codec + NS4150B speaker
│   ├── mic.c               # ES7210 dual microphone
│   ├── imu.c/h             # BMI270 6-axis IMU
│   ├── rtc.c/h             # RX8130CE real-time clock
│   ├── battery.c/h         # INA226 battery monitor
│   └── bluetooth.c/h       # BLE stub (ESP-Hosted pending)
```

## Known Issues

- **ESP-IDF v5.5.x** breaks DSI display (stripes/empty). Use v5.4.2.
- **SD Card + WiFi conflict**: SD uses SPI which can conflict. Power-cycle workaround needed.
- **Double boot**: Normal on first flash — PSRAM timing calibration triggers reset.
- **Arduino v3.3.x**: Inherits IDF v5.5 DSI bug. Use v3.2.1.

## Resources

- [M5Stack Tab5 Docs](https://docs.m5stack.com/en/core/Tab5)
- [Factory Firmware Guide](https://docs.m5stack.com/en/esp_idf/m5tab5/userdemo)
- [ESPP Tab5 Class](https://esp-cpp.github.io/espp/m5stack_tab5.html)
- [ESPHome Tab5](https://devices.esphome.io/devices/m5stack-tab5/)
- [CNX Review Part 1](https://www.cnx-software.com/2025/05/14/m5stack-tab5-review-part-1-unboxing-teardown-and-first-try-of-the-esp32-p4-and-esp32-c6-5-inch-iot-devkit/)
- [CNX Review Part 2](https://www.cnx-software.com/2025/05/18/m5stack-tab5-review-getting-started-esp32-p4-esp-idf-framework-arduino-ide/)

## License

MIT
