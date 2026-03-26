# TinkerTab — Build Plan

Systematic build of the TinkerTab firmware for M5Stack Tab5 (ESP32-P4).
Every phase is documented, committed, and pushed chronologically.

## Hardware Target

| Component | IC | Interface | Status |
|-----------|-----|-----------|--------|
| Main SoC | ESP32-P4 (RISC-V dual 400MHz) | — | ✅ |
| Display | ST7123 TDDI 720x1280 | MIPI-DSI 2-lane | ✅ Phase 1 |
| Touch | ST7123 integrated | I2C 0x55 | ✅ Phase 1 |
| IO Expander 1 | PI4IOE5V6416 | I2C 0x43 | ✅ Phase 1 |
| IO Expander 2 | PI4IOE5V6416 | I2C 0x44 | ✅ Phase 1 |
| Backlight | GPIO 22 LEDC | PWM 5kHz | ✅ Phase 1 |
| WiFi/BLE | ESP32-C6-MINI-1U | SDIO 4-bit (ESP-Hosted) | ✅ Phase 1 |
| SD Card | — | SDMMC 4-bit | 🔲 Phase 2 |
| Camera | SC2356 2MP | MIPI-CSI | 🔲 Phase 3 |
| Audio Codec | ES8388 | I2S + I2C control | 🔲 Phase 4 |
| Audio ADC | ES7210 (dual mic) | I2S + I2C control | 🔲 Phase 5 |
| IMU | BMI270 6-axis | I2C / SPI | 🔲 Phase 6 |
| RTC | RX8130CE | I2C | 🔲 Phase 7 |
| Battery Monitor | INA226 | I2C | 🔲 Phase 8 |
| Charger IC | IP2326 | — (managed by IO expander) | 🔲 Phase 8 |
| Speaker Amp | NS4150B 1W | GPIO enable + I2S DAC | 🔲 Phase 4 |

## Build Phases

### Phase 0 — Repo + Project Setup ✅
- [x] Create GitHub repo
- [x] ESP-IDF project scaffold (CMakeLists, sdkconfig.defaults, partitions)
- [x] Project README with hardware overview
- [x] Build plan document (this file)
- [x] .gitignore for ESP-IDF

### Phase 1 — Port Existing Drivers (from pingdev/tab5)
- [ ] I2C bus init
- [ ] IO expander driver (PI4IOE5V6416 x2)
- [ ] MIPI DSI display (ST7123)
- [ ] Backlight (LEDC PWM)
- [ ] Touch (ST7123 TDDI)
- [ ] WiFi (ESP-Hosted SDIO → ESP32-C6)
- [ ] Config header with all pin definitions
- [ ] Kconfig for runtime configuration
- [ ] Serial command interface
- [ ] HW JPEG decoder
- **Commit:** "feat: Phase 1 — port display, touch, WiFi, IO expanders from pingdev"

### Phase 2 — SD Card (SDMMC)
- [ ] SDMMC 4-bit driver init
- [ ] Mount FAT filesystem
- [ ] Read/write test
- [ ] Serial commands: `sd info`, `sd ls`, `sd cat <file>`
- [ ] Document pin mapping and known WiFi conflict workaround
- **Commit:** "feat: Phase 2 — SD card SDMMC 4-bit driver"

### Phase 3 — Camera (SC2356 MIPI-CSI)
- [ ] MIPI-CSI interface init
- [ ] SC2356 I2C configuration
- [ ] Single frame capture (JPEG)
- [ ] Save to SD card
- [ ] Serial commands: `cam snap`, `cam info`
- [ ] Live viewfinder mode (decode to display)
- **Commit:** "feat: Phase 3 — camera SC2356 MIPI-CSI driver"

### Phase 4 — Audio Codec (ES8388 + Speaker)
- [ ] I2S bus init (I2S_NUM_0)
- [ ] ES8388 I2C configuration
- [ ] NS4150B speaker amp enable (via IO expander)
- [ ] WAV playback from SD card
- [ ] Volume control
- [ ] Serial commands: `audio play <file>`, `audio vol <0-100>`, `audio stop`
- **Commit:** "feat: Phase 4 — audio codec ES8388 + speaker playback"

### Phase 5 — Audio ADC / Microphone (ES7210)
- [ ] ES7210 I2C configuration
- [ ] Dual mic capture (I2S input)
- [ ] Record to WAV on SD card
- [ ] Audio level meter
- [ ] Serial commands: `mic record <seconds>`, `mic level`
- [ ] AEC test (play + record simultaneously)
- **Commit:** "feat: Phase 5 — microphone ES7210 dual mic recording"

### Phase 6 — IMU (BMI270)
- [ ] BMI270 I2C/SPI init
- [ ] Read accelerometer (x, y, z)
- [ ] Read gyroscope (x, y, z)
- [ ] Orientation detection (portrait/landscape)
- [ ] Tap/shake detection
- [ ] Serial commands: `imu read`, `imu orient`, `imu tap`
- **Commit:** "feat: Phase 6 — IMU BMI270 6-axis driver"

### Phase 7 — RTC (RX8130CE)
- [ ] RX8130CE I2C init
- [ ] Read date/time
- [ ] Set date/time
- [ ] Alarm configuration
- [ ] Sync from NTP when WiFi available
- [ ] Serial commands: `rtc get`, `rtc set <datetime>`, `rtc alarm <time>`
- **Commit:** "feat: Phase 7 — RTC RX8130CE driver"

### Phase 8 — Battery & Power (INA226 + IP2326)
- [ ] INA226 I2C init
- [ ] Read voltage, current, power
- [ ] Battery percentage estimation
- [ ] Charging status detection (via IO expander / IP2326)
- [ ] Low battery warning
- [ ] Serial commands: `bat info`, `bat voltage`, `bat charging`
- **Commit:** "feat: Phase 8 — battery monitor INA226 + charging status"

### Phase 9 — LVGL Integration
- [ ] LVGL v9 integration with MIPI DSI display
- [ ] Touch input driver for LVGL
- [ ] Theme setup (dark mode)
- [ ] Basic screen rendering test
- [ ] Font setup (optimized for 720px width)
- **Commit:** "feat: Phase 9 — LVGL v9 integration with display + touch"

### Phase 10 — Native Launcher UI
- [ ] Boot splash screen
- [ ] Home screen (clock, app grid, status bar)
- [ ] Settings screen (WiFi, brightness, battery, about)
- [ ] Camera viewfinder screen
- [ ] File browser (SD card)
- [ ] Audio player screen
- **Commit:** "feat: Phase 10 — native LVGL launcher UI"

### Phase 11 — Dragon Mode
- [ ] MJPEG streaming client
- [ ] WebSocket touch forwarding
- [ ] Auto-detect Dragon on network (mDNS)
- [ ] Seamless mode switching (LVGL ↔ MJPEG)
- [ ] Dragon server (python, from pingdev)
- **Commit:** "feat: Phase 11 — Dragon mode MJPEG + touch forwarding"

## Pin Reference

See `main/config.h` for complete pin mapping.

## Dependencies

- ESP-IDF v5.4.2 (v5.5.x has DSI display bug #18083)
- esp_lcd_touch_st7123 ≥1.0.0
- espressif/esp_hosted 1.4.0
- espressif/esp_wifi_remote 0.8.5
- LVGL v9 (Phase 9+)
