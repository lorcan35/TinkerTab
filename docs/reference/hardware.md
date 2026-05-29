---
audience: integrator
type: reference
prerequisites: none
last-verified: 2026-05-29
---
# Hardware reference

Authoritative, lookup-oriented reference for the [Tab5](../../GLOSSARY.md)
hardware the firmware targets. No tutorials here. All pin numbers are GPIO
numbers on the ESP32-P4 SoC unless noted. The single source of truth is
[`bsp/tab5/bsp_config.h`](../../bsp/tab5/bsp_config.h) — if a value here disagrees
with that header, the header wins. For the full bus map, register details, and
boot sequence see [`docs/HARDWARE.md`](../HARDWARE.md).

## SoC: ESP32-P4

| Field | Value |
|---|---|
| Architecture | RISC-V (rv32imafc) dual-core HP @ 360–400 MHz + 1 LP core |
| Internal SRAM | ~512 KB (shared with FreeRTOS) — fragments under overlay create/destroy |
| PSRAM | 32 MB Octal SPI — LVGL pool, mic buffers, media cache, video frames, JPEG scratch |
| Flash | 16 MB QuadSPI — partition table in [`partitions.csv`](../../partitions.csv) (dual OTA slots) |
| Crypto | HW AES-256, SHA, RSA, HMAC (mbedTLS for WS-TLS) |
| JPEG engine | Single HW codec ("rxlink") — shared by call streamer + camera recording, mutex-guarded |
| Wi-Fi / BT | **None native** — provided by a hosted ESP32-C6 over SDIO |
| ESP-IDF | Pinned to **v5.5.2** (`dependencies.lock`) |

## Peripherals

| Subsystem | Part | Interface | Key facts |
|---|---|---|---|
| Display | ST7123 panel | MIPI DSI, 2 lanes | 720×1280 IPS, RGB565→RGB888, ~60 fps (LVGL flushes ~10 fps under load); backlight PWM on GPIO 22 |
| Touch | GT911 | I2C | capacitive |
| Camera | SC202CS (a.k.a. SC2336) | MIPI-CSI, 1-lane | 2 MP, SCCB address `0x36` (**not** `0x30`), RAW8 → ISP → RGB565, 1280×720 @ 30 fps; needs `CONFIG_CAMERA_SC202CS=y` |
| Audio DAC | ES8388 | I2S TX (STD Philips) | speaker out; init via `es8388_codec_new()` only (never custom registers) |
| Audio ADC | ES7210 | I2S RX (TDM 4-slot) | 4-mic array; slot 0 = MIC-L primary; both codecs share I2S_NUM_1 |
| Wi-Fi | ESP32-C6 | SDIO | hosted co-processor (P4 has no native radio) |
| SD card | SDMMC | 4-bit, SLOT 0 | FAT32 in 8.3 short-name mode (`CONFIG_FATFS_LFN_NONE=1`); LDO channel 4; coexists with Wi-Fi SDIO |
| IMU | BMI270 | I2C | tilt / motion (orb specular) |
| I/O expanders | 2× PI4IOE5V6416 | I2C | power rails, peripheral enables |

## Audio pipeline facts

| Item | Value |
|---|---|
| I2S mode | Mixed: TX = STD Philips (ES8388), RX = TDM 4-slot (ES7210), both on `I2S_NUM_1` |
| Hardware rate | 48 kHz |
| STT rate | 16 kHz — downsample 3:1 from 48 kHz |
| TTS playback | upsample 1:3 from 16 kHz to 48 kHz |
| Codec I2C addresses | 8-bit: ES7210 = `0x80`, ES8388 = `0x20` (**not** 7-bit) |
| Slot mode | `I2S_SLOT_MODE_STEREO` for TDM multi-slot capture; MONO gets only slot 0 |

## M5-Bus rear connector (K144 / TinkerON)

| Item | Value |
|---|---|
| Carrier | Module13.2 LLM Mate required between Tab5 and K144 (direct stack collides 5V rails) |
| Stack order | `Tab5 base → Mate (USB-C powered) → K144` |
| UART | `UART_NUM_1`, TX = GPIO 6, RX = GPIO 7 (Port C UART, M5-Bus pins 16/15) |
| Baud | 115200 8N1 for setup; bumps to 1.5 Mbps for the ext_pcm pump |
| Avoid | UART0 (G37/G38) — collides with `idf.py monitor` |
| Power | Tab5's own USB-C powers the whole stack via M5-Bus (~1.5 W at K144 full load) |

## ESP32-P4 memory rules

- **PSRAM for large buffers (>4 KB):**
  `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`. Free with
  `heap_caps_free()`, never `free()`.
- **Never `vTaskDelete(NULL)`** — use `vTaskSuspend(NULL)` (P4 TLSP cleanup
  crash).
- **`heap_caps_get_free_size()` lies** — total free ≠ usable. Use
  `heap_caps_get_largest_free_block()` for the largest contiguous block;
  fragmentation makes total free look healthy while allocations fail.
- **Stack sizes:** SDIO tasks 8 K+, mic task 4 K, WS task 8 K. All tasks doing
  network + LVGL callbacks need ≥8 K.
- **PSRAM cache coherency:** DPI DMA reads PSRAM directly — call
  `esp_cache_msync()` after CPU writes to the framebuffer.

## NVS layout

Settings live in the `"settings"` [NVS](../../GLOSSARY.md) namespace (max key
length 15 chars). See the [NVS settings reference](nvs-settings.md) for the full
key table.

## Power

| Item | Value |
|---|---|
| Battery | 6 Ah LiPo |
| Charge / fuel gauge | via the I2C bus (see `bsp_config.h`) |

## See also

- Full pinout + bus map + boot sequence: [`docs/HARDWARE.md`](../HARDWARE.md)
- [Hardware mods](../hardware-mods.md) · [Firmware file map](firmware-file-map.md)
- [TinkerTab architecture](../explanation/architecture.md) ·
  [LVGL on ESP32-P4](../explanation/lvgl-on-esp32p4.md)
