# Tab5 Hardware Reference

> Single-source pinout, IC list, and bus map for the M5Stack Tab5
> hardware Tab5 firmware targets.  All pin numbers are GPIO numbers
> on the ESP32-P4 SoC unless noted otherwise.  Authoritative
> source: [`bsp/tab5/bsp_config.h`](../bsp/tab5/bsp_config.h) — if
> this doc disagrees with that file, the C header wins.

## SoC: ESP32-P4

| | |
|---|---|
| **Architecture** | RISC-V (rv32imafc) dual-core HP @ 360-400 MHz + 1 LP core |
| **Internal SRAM** | ~512 KB (shared with FreeRTOS) — fragments under overlay create/destroy |
| **PSRAM** | 32 MB (Octal SPI) — used for LVGL pool, mic buffers, media cache, video frames, JPEG scratch |
| **Flash** | 16 MB QuadSPI — partition table in `partitions.csv` (dual OTA slots) |
| **Crypto** | HW AES-256, SHA, RSA, HMAC — used by mbedTLS for WS-TLS |
| **JPEG engine** | Single HW JPEG codec ("rxlink") — shared by call streamer + camera recording (PR #291). Mutex-guarded; second `jpeg_new_encoder_engine()` call fails. |

The ESP32-P4 has no built-in WiFi or Bluetooth — those go through a hosted ESP32-C6 over SDIO (see below).

## ESP-IDF version

Pinned to **v5.5.2** (`dependencies.lock`). After any sdkconfig change or component update, run `idf.py fullclean build`.

## Display: 5" 720×1280 IPS

| | |
|---|---|
| **Panel driver IC** | ST7123 |
| **Interface** | MIPI DSI |
| **Lanes** | 2 |
| **Bitrate per lane** | 965 Mbps |
| **DPI clock** | 70 MHz |
| **Pixel format** | RGB565 (DSI peripheral) → RGB888 (panel) |
| **Refresh** | ~60 fps (LVGL flushes at ~10 fps under load — see `/info.lvgl_fps`) |
| **HSYNC / HBP / HFP** | 2 / 40 / 40 |
| **VSYNC / VBP / VFP** | 2 / 8 / 220 |
| **Backlight** | PWM on GPIO 22, 5 kHz |
| **PHY LDO** | Channel 3, 2500 mV |

LVGL config — `LV_DISPLAY_RENDER_MODE_PARTIAL` with two 144 KB draw buffers in PSRAM.  Do NOT use DIRECT mode (causes tearing on DPI).

## Touch: GT911 capacitive

| | |
|---|---|
| **Bus** | System I2C (SDA=31, SCL=32, 400 kHz) |
| **Interrupt** | GPIO 23 |
| **Driver** | `esp_lcd_touch_st7123` component (mis-named — actually wraps GT911) |

Forwarded as LVGL pointer events.  Coordinates: `(0,0)` = top-left, max `(719, 1279)`.

## Camera: SC202CS (SC2336) 2 MP

| | |
|---|---|
| **Sensor** | SC202CS — 2 MP rolling-shutter CMOS |
| **Interface** | MIPI-CSI, 2 lanes |
| **External clock** | 24 MHz on GPIO 36 |
| **SCCB I2C address** | 0x36 (NOT 0x30) — `CONFIG_CAMERA_SC202CS=y` must be set in sdkconfig |
| **Driver stack** | `esp_video` + `esp_cam_sensor` (M5Stack-derived; not the raw CSI API) |
| **Native output** | 1280×720 RGB565 @ 30 fps |
| **ISP path** | RAW8 → ISP → RGB565 → /dev/video0 (V4L2) |

Software rotation via NVS `cam_rot` (0/1/2/3 for 0°/90°/180°/270° CW) is applied to every captured frame before display, photo save, video recording, and Dragon upload.  See `main/ui_camera.c` and PR #261/#290.

## Audio: ES8388 DAC + ES7210 ADC, shared I2S

I2S_NUM_1 carries both TX (DAC) and RX (ADC) in **TDM 4-slot mode** to keep BCLK consistent at 48 kHz × 4 × 16 = 3.072 MHz.  An older mixed-mode (TX=STD, RX=TDM) caused the legendary "[MUSIC PLAYING]" buzzing bug.

| | |
|---|---|
| **MCLK** | GPIO 30 (shared) |
| **BCLK** | GPIO 27 (shared) |
| **WS / LRCK** | GPIO 29 (shared) |
| **DOUT → ES8388** | GPIO 26 |
| **DIN ← ES7210** | GPIO 28 |
| **Native rate** | 48 kHz |
| **TX slot mode** | STD Philips → ES8388 |
| **RX slot mode** | TDM 4-slot → ES7210 (slot 0 = MIC-L primary, slots 1-3 = secondary mics for AEC reference, currently unused) |
| **ES8388 I2C addr** | 0x10 (7-bit), 0x20 (8-bit) |
| **ES7210 I2C addr** | 0x40 (7-bit), 0x80 (8-bit) |
| **Speaker enable** | IO Expander 1 P1 (initialized LOW at boot to prevent buzz) |

Audio is ALWAYS through `esp_codec_dev_*` APIs.  Never write custom register sequences — see LEARNINGS for the 5-difference disaster that prevented audio (issue #46).

The voice pipeline downsamples 3:1 (48 kHz → 16 kHz) for STT and upsamples 1:3 for TTS playback — see [`docs/VOICE_PIPELINE.md`](VOICE_PIPELINE.md).

## WiFi: ESP32-C6 hosted via SDIO

ESP32-P4 has no native WiFi.  An on-board ESP32-C6 acts as the radio over the ESP-Hosted protocol on SDIO 4-bit.

| | |
|---|---|
| **CLK** | GPIO 12 |
| **CMD** | GPIO 13 |
| **D0** | GPIO 11 |
| **D1** | GPIO 10 |
| **D2** | GPIO 9 |
| **D3** | GPIO 8 |
| **RESET** | GPIO 15 |
| **Power** | IO Expander 2 controlled |

Connection mode (NVS `conn_m`): `0=auto` (ngrok-first then LAN), `1=local-only`, `2=remote-only`.  Backoff is exponential-with-full-jitter, capped at 60 s.

## SD Card: SDMMC 4-bit

| | |
|---|---|
| **CLK** | GPIO 43 |
| **CMD** | GPIO 44 |
| **D0** | GPIO 39 |
| **D1** | GPIO 40 |
| **D2** | GPIO 41 |
| **D3** | GPIO 42 |
| **Slot** | 0 (with LDO channel 4) |
| **Filesystem** | FAT32 (`CONFIG_FATFS_LFN_NONE=1` — 8.3 short names only — see LEARNINGS for the .MJP file extension hack) |

SD coexists with WiFi SDIO on a different controller.  Card is hot-plug-aware; the camera screen re-scans for `IMG_NNNN.jpg` on every open if the card was just inserted.

## I/O Expanders: 2× PI4IOE5V6416

System I2C, addresses 0x43 + 0x44.

| Expander 1 (0x43) | Pin | Use |
|-------------------|-----|-----|
| P0 | LCD reset |
| P1 | Speaker enable (active-LOW init) |
| P2 | Touch reset |
| P3 | Camera reset |
| P4 | External 5 V power |
| ... | ... |

| Expander 2 (0x44) | Pin | Use |
|-------------------|-----|-----|
| P0 | WiFi (ESP32-C6) power |
| P1 | USB host 5 V |
| P2 | Battery charging enable |
| ... | ... |

## M5-Bus rear connector

30-pin pinheader on the back of the Tab5, hidden behind the battery cover.  Used for stacked M5Stack modules (K144 LLM Module Kit, etc.) — see [`docs/PLAN-m5-llm-module.md`](PLAN-m5-llm-module.md) and [`docs/PLAN-grove.md`](PLAN-grove.md) for the active integration plans.  Authoritative source: the official M5Stack [Tab5 schematic PDF](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf).

| M5-Bus pin | Tab5 signal | ESP32-P4 GPIO | Notes |
|---|---|---|---|
| 1 | GND | — | |
| 2 | GPIO | G16 | General-purpose, currently unused |
| 3 | GND | — | |
| 4 | PB_IN | G17 | Power-button input chain |
| 5 | GND | — | |
| 6 | EN / RST | — | Tab5 reset line — driven by reset switch |
| 7 | SPI MOSI | G18 | Shared with internal SPI |
| 8 | GPIO | G45 | Currently unused |
| 9 | SPI MISO | G19 | Shared with internal SPI |
| 10 | PB_OUT | G52 | Power-button output chain |
| 11 | SPI SCK | G5 | Shared with internal SPI |
| 12 | 3V3 | — | |
| 13 | RXD0 | G38 | UART0 RX — collides with the boot console; **don't use for new modules** |
| 14 | TXD0 | G37 | UART0 TX — same caveat |
| 15 | PC_RX | **G7** | Port C UART RX (also exposed at side 4-pin Port C header — same wires) |
| 16 | PC_TX | **G6** | Port C UART TX (also exposed at side 4-pin Port C header — same wires) |
| 17 | Internal SDA | G31 | System I2C — same bus as touch / IO expanders / IMU |
| 18 | Internal SCL | G32 | System I2C — same bus as touch / IO expanders / IMU |
| 19 | GPIO | G3 | Currently unused |
| 20 | GPIO | G4 | Currently unused |
| 21 | GPIO | G2 | Currently unused |
| 22 | GPIO | G48 | Currently unused |
| 23 | GPIO | G47 | Currently unused |
| 24 | GPIO | G35 | Currently unused |
| 25 | HVIN | — | High-voltage input (M5-Bus power-injection) |
| 26 | GPIO | G51 | Currently unused |
| 27 | HVIN | — | |
| 28 | EXT_5V_BUS | — | External 5V to all expansion connectors; gated by `EXT5V_EN` on IO-Expander 1, pin P4 |
| 29 | HVIN | — | |
| 30 | BAT | — | Direct battery rail |

**Special signals:**
- `EXT_5V_BUS` (pin 28) is OFF by default at boot.  Driving it requires asserting `EXT5V_EN` (IO-Expander 1, P4).  Same gate also powers Port A (HY2.0-4P) and side Port C 5V — flipping it energises ALL expansion 5V rails simultaneously.  Implication: the K144 stacked LLM module won't power up via M5-Bus until firmware enables EXT5V_EN — bench rigs sidestep by plugging the K144's top USB-C in for independent power.
- `RF_PTH_L_INT_H_EXT` (not exposed on the 30-pin header but referenced in the schematic) selects internal vs SMA antenna for the ESP32-C6 radio.
- `WLAN_PWR_EN` (IO-Expander 2, pin P0) gates power to the ESP32-C6 — orthogonal to M5-Bus modules but listed here because it shares the same expander logic.

**UART selection rules:**
- For new modules: use **GPIO 6 (TX) / GPIO 7 (RX) on UART_NUM_1** (Port C UART, M5-Bus pins 15/16).  Same UART is exposed on the side Port C 4-pin header so the firmware works whether the module is stacked or wired via Grove cable.
- Avoid UART0 on M5-Bus pins 13/14 (G37/G38) — collides with `idf.py monitor` boot console.
- UART_NUM_2 stays free for a future second module.

## Sensors

| Sensor | I2C addr | Use |
|--------|----------|-----|
| **BMI270** IMU | 0x68 | 6-axis accel + gyro; auto-rotate (NVS-toggleable, currently no-op pending #287) |
| **RX8130CE** RTC | 0x32 | Wall-clock; battery-backed; survives reboots |
| **INA226** battery monitor | 0x41 | Voltage/current monitoring; A0 routed high to avoid clash with ES7210 at 0x40 |

## Battery

LiPo 6 Ah, monitored via INA226.  Charging routed through Expander 2.  Tab5 reports voltage + percentage via `GET /battery` debug endpoint.

## NVS layout

NVS partition lives at flash offset `0x9000`, size `0x6000`.  Namespace `"settings"`.  Max key length 15 chars.  See [`CLAUDE.md`](../CLAUDE.md) "NVS Settings Keys" section for the canonical key list with types/ranges/defaults.

Wear-leveling: most keys are written rarely (mode change, settings UI), but `spent_mils` writes once per LLM turn — bounded to ~hundreds of writes/day, well under the 100k cycle endurance.

## Boot sequence (high-level)

`main/main.c:app_main()`:

1. NVS open + auth_tok generation if missing
2. I2C bus + IO expanders + battery + RTC + IMU
3. WiFi via ESP-Hosted on SDIO; STA mode connects
4. SD card mount (FAT32)
5. Audio codec (ES8388 + ES7210) via esp_codec_dev
6. Camera (SC202CS via esp_video stack)
7. LVGL display + touch
8. UI scaffolding: theme, widget store, chat store, voice overlay, home screen, nav bar
9. `tab5_debug_obs_init()` + debug HTTP server on port 8080
10. `tab5_worker_init()` (shared FreeRTOS job queue)
11. Voice WS client (Dragon connection)
12. Heap watchdog (3-min sustained-low-largest-block reboot trigger)

Total time-to-ready: ~25-35 s on a fresh boot, ~15-20 s on a watchdog reboot.

## Power consumption

| State | Current draw (5 V) |
|-------|---------------------|
| Idle, screen on, WiFi connected | ~250-300 mA |
| Active LLM call (cloud) | ~400 mA |
| Recording video (5 fps) | ~450 mA |
| Screen off (TODO — sleep mode not yet implemented) | n/a |

6 Ah battery → ~20 hours active use, much longer on idle if power management lands.

## Hardware-related issues

See [`LEARNINGS.md`](../LEARNINGS.md) for the full list of hardware quirks we've hit.  Highlights:
- ES8388 init must use `esp_codec_dev_new()` — custom register writes have 5 differences vs library and prevent audio entirely
- I2S TDM 4-slot for both TX and RX (BCLK consistency)
- SC202CS at 0x36 (NOT 0x30 like SC2336)
- `vTaskDelete(NULL)` crashes on P4 — use `vTaskSuspend(NULL)` instead
- DMA cache coherency on PSRAM: `esp_cache_msync()` after CPU writes to framebuffer
- Single HW JPEG engine — must share via `voice_video_encode_rgb565()`
- DMA-aligned buffers via `jpeg_alloc_encoder_mem()` — plain `heap_caps_malloc(MALLOC_CAP_DMA)` doesn't satisfy alignment

## Further reading

- [`../bsp/tab5/bsp_config.h`](../bsp/tab5/bsp_config.h) — authoritative pinout
- [`../CLAUDE.md`](../CLAUDE.md) — operational reference + NVS keys + debug endpoints
- [`../LEARNINGS.md`](../LEARNINGS.md) — hardware gotchas
- [`VOICE_PIPELINE.md`](VOICE_PIPELINE.md) — audio chain details
- [TinkerBox `docs/ARCHITECTURE.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/ARCHITECTURE.md) — Dragon-side architecture
