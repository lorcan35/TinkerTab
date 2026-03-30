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
| SD Card | — | SDMMC 4-bit | ✅ Phase 2 |
| Camera | SC202CS (SC2356) 2MP | MIPI-CSI 2-lane | ✅ Phase 3 |
| Audio Codec | ES8388 | I2S + I2C control | ✅ Phase 4 |
| Speaker Amp | NS4150B 1W | GPIO enable + I2S DAC | ✅ Phase 4 |
| Audio ADC | ES7210 (dual mic) | I2S + I2C control | ✅ Phase 5 |
| IMU | BMI270 6-axis | I2C 0x68 | ✅ Phase 6 |
| RTC | RX8130CE | I2C 0x32 | ✅ Phase 7 |
| Battery Monitor | INA226 | I2C 0x40 | ✅ Phase 8 |
| Charger IC | IP2326 | IO expander | ✅ Phase 8 |
| BLE | via ESP32-C6 | ESP-Hosted | ⚠️ Stub (SDK pending) |

## Build Phases

### Phase 0 — Repo + Project Setup ✅
- [x] Create GitHub repo
- [x] ESP-IDF project scaffold (CMakeLists, sdkconfig.defaults, partitions)
- [x] Project README with hardware overview
- [x] Build plan document (this file)
- [x] .gitignore for ESP-IDF

### Phase 1 — Port Existing Drivers ✅
- [x] I2C bus init
- [x] IO expander driver (PI4IOE5V6416 x2)
- [x] MIPI DSI display (ST7123)
- [x] Backlight (LEDC PWM)
- [x] Touch (ST7123 TDDI)
- [x] WiFi (ESP-Hosted SDIO → ESP32-C6)
- [x] Config header with all pin definitions
- [x] Kconfig for runtime configuration
- [x] Serial command interface
- [x] HW JPEG decoder
- **Commit:** `feat: Phase 1 — port display, touch, WiFi, IO expanders from pingdev`

### Phase 2 — SD Card (SDMMC) ✅
- [x] SDMMC 4-bit driver init
- [x] Mount FAT filesystem at /sdcard
- [x] Capacity / free space queries
- [x] Serial command: `sd`
- **Commit:** `feat: Phase 2 — SD card SDMMC 4-bit driver`

### Phase 3 — Camera (SC202CS MIPI-CSI) ✅
- [x] XCLK via LEDC on GPIO 36 @ 24MHz
- [x] SCCB (I2C) register access at 0x36 (SC202CS, NOT SC2336)
- [x] Chip ID verification (0xEB52)
- [x] MIPI-CSI controller init (2 data lanes)
- [x] Single frame capture to PSRAM
- [x] Save to SD card
- [x] Camera reset via IO expander P6
- [x] Serial command: `cam`
- **Commit:** `feat: Phase 3 — camera SC2336 MIPI-CSI driver`

### Phase 4 — Audio Codec (ES8388 + Speaker) ✅
- [x] I2S TX channel init
- [x] ES8388 I2C configuration
- [x] NS4150B speaker amp enable (via IO expander P1)
- [x] Raw PCM playback
- [x] Volume control (0-100)
- [x] Serial command: `audio` (440Hz test tone)
- **Commit:** `feat: Phase 4 — audio codec ES8388 + speaker playback`

### Phase 5 — Microphone (ES7210) ✅
- [x] ES7210 via esp_codec_dev (replaced hand-rolled register writes)
- [x] I2S RX TDM 4-slot on shared bus (full-duplex with TX)
- [x] Quad mic capture (MIC1/AEC/MIC2/MIC-HP in 4 TDM slots)
- [x] Gain control (0-37 dB)
- [x] Serial commands: `mic` (1s recording + RMS), `micdiag` (per-channel stats)
- **Commit:** `feat: Phase 5 — microphone ES7210 dual mic recording`

### Phase 6 — IMU (BMI270) ✅
- [x] BMI270 I2C init + chip ID verify
- [x] Accelerometer (±4g, 100Hz)
- [x] Gyroscope (±2000 dps, 100Hz)
- [x] Orientation detection (portrait/landscape)
- [x] Tap/shake gesture detection
- [x] Serial command: `imu`
- **Commit:** `feat: Phase 6 — IMU BMI270 6-axis driver`

### Phase 7 — RTC (RX8130CE) ✅
- [x] RX8130CE I2C init
- [x] Read / set date/time
- [x] NTP sync over WiFi
- [x] Serial commands: `rtc`, `ntp`
- **Commit:** `feat: Phase 7 — RTC RX8130CE driver`

### Phase 8 — Battery & Power (INA226) ✅
- [x] INA226 I2C init + calibration
- [x] Voltage, current, power readings
- [x] Battery percentage estimation (2S LiPo curve)
- [x] Charging status detection
- [x] Serial command: `bat`
- **Commit:** `feat: Phase 8 — battery monitor INA226 + charging status`

### Phase 8b — BLE (Stub) ✅
- [x] Stub driver that returns ESP_ERR_NOT_SUPPORTED
- [x] Documented: ESP-Hosted doesn't forward BLE yet
- **Commit:** included with Phase 8

### Phase 9 — LVGL Integration ✅
- [x] LVGL v9 with DPI framebuffer flush + cache_msync
- [x] Touch input driver (ST7123 → LVGL pointer)
- [x] Dark theme with #3B82F6 accent
- [x] Montserrat fonts (14/20/28/36)
- [x] Double-buffered 144KB draw buffers in PSRAM
- [x] FreeRTOS UI task on core 0 with mutex
- **Commit:** `feat: Phase 9 — LVGL v9 core integration`

### Phase 10 — Native Launcher UI ✅
- [x] Boot splash (logo, progress bar, status text)
- [x] Home screen (status bar, clock, 3x3 app grid, dock)
- [x] Settings screen (brightness, WiFi, BLE, NTP, storage, battery, about)
- [x] Camera viewfinder (live preview, capture, resolution switch)
- [x] File browser (SD card directory listing, file type icons)
- [x] Audio player (WAV playback, play/pause, volume)
- [x] Splash → home transition with progress during boot
- **Commits:** `Phase 10a-10e`

### Phase 11 — Dragon Mode ✅ (from Phase 1)
- [x] MJPEG streaming client (ported from pingdev)
- [x] WebSocket touch forwarding (ported from pingdev)
- [x] Auto-detect Dragon on network (mDNS `_tinkerclaw._tcp`)
- [x] Mode manager FSM (IDLE/STREAMING/VOICE/BROWSING) — coordinates exclusive WiFi bandwidth

### Phase 12 — Voice Pipeline (STT/TTS/LLM)
**Status: Phase 1 complete (Moonshine v2 + Ollama + Piper). Streaming pipeline (12c) and wake word (12d) pending.**

#### 12a: Dragon Voice Server ✅
- [x] Python WebSocket server on port 3502
- [x] Swappable STT backends (Moonshine, Whisper.cpp, Vosk)
- [x] Swappable TTS backends (Piper, Kokoro, Edge TTS)
- [x] Swappable LLM backends (Ollama, OpenRouter, LM Studio)
- [x] Config-driven via config.yaml with env var overrides
- [x] STT backend factory with abstract base class
- [x] Whisper.cpp backend implementation

#### 12b: Tab5 Voice Streaming ✅
- [x] WebSocket client connecting to Dragon voice server (port 3502, /ws/voice)
- [x] Mic capture: 4-ch TDM 48kHz -> extract slot 0 -> downsample 3:1 to 16kHz mono -> WS binary
- [x] Receive TTS audio: 16kHz PCM -> upsample 3x to 48kHz -> ring buffer -> ES8388 playback
- [x] Push-to-talk state machine (IDLE/CONNECTING/READY/LISTENING/PROCESSING/SPEAKING)
- [x] Voice UI overlay (animated orb, mic button, transcript display)
- [x] Serial command: `voice` (spawns voice test task)
- [x] JSON keepalive heartbeat (prevents TCP idle timeout during LLM processing)
- [x] Playback ring buffer in PSRAM (smooths network jitter)

#### 12c: Streaming Pipeline (Phase 2) ⬜
- [ ] OPUS encoding/decoding on both ends
- [ ] Moonshine v2 streaming STT
- [ ] Sentence-level TTS streaming
- [ ] VAD on Tab5 (ESP-SR VADNet)

#### 12d: Wake Word + Production (Phase 3) ⬜
- [ ] ESP-SR WakeNet9 / Porcupine for "Hey Glyph"
- [ ] Acoustic Echo Cancellation (AEC)
- [ ] Barge-in support
- [ ] Kokoro TTS upgrade

### Phase 13 — WiFi Config + NVS Settings ✅
- [x] WiFi config screen (scan networks, tap to connect, on-screen keyboard)
- [x] NVS settings persistence (WiFi, Dragon host, brightness, volume)
- [x] Settings loaded at boot, applied before UI starts
- [x] On-screen keyboard with lazy-loaded number row
- **Commit:** `feat: Phase 1 — mDNS discovery, WiFi config screen, NVS settings persistence`

### Phase 14 — Debug Server ✅
- [x] HTTP server on port 8080
- [x] Endpoints: `/screenshot`, `/info`, `/touch`, `/reboot`, `/log`, `/` (dashboard)
- [x] Remote screenshot capture (JPEG from framebuffer)
- [x] Remote touch injection
- [x] mDNS hostname: `tinkertab.local`
- **Commit:** `feat: add HTTP debug server — screenshots, device info, remote touch`

### Phase 15 — UI Sizing + Bug Fixes ✅
- [x] All fonts/buttons/touch targets scaled for 720x1280 DPI
- [x] 17 bug fixes: LVGL thread safety, voice pipeline hardening, app grid wiring
- [x] LVGL overlay crash fix (deferred + lazy number rows)
- [x] MJPEG flickering fix (gate decode to Dragon page only)
- [x] Service registry — layered init/start/stop lifecycle for all subsystems
- [x] Mode manager — FSM coordinates exclusive WiFi bandwidth (IDLE/STREAMING/VOICE/BROWSING)
- **Commits:** `fix: comprehensive UI sizing overhaul`, `fix: 17 bug fixes`

## Pin Reference

See `main/config.h` for complete pin mapping.

## Dependencies

- ESP-IDF v5.4.3 (v5.4.2 missing PSRAM XIP/TCM fixes; v5.5.x has DSI bug #18083)
- esp_lcd_touch_st7123 ≥1.0.0
- espressif/esp_hosted 1.4.0
- espressif/esp_wifi_remote 0.8.5
- LVGL v9 (Phase 9+)
