---
title: Specs at a glance
sidebar_label: Specs at a glance
---

# Specs at a glance

A condensed datasheet for the M5Stack Tab5. Full pinout in [the TinkerTab `docs/HARDWARE.md`](https://github.com/lorcan35/TinkerTab/blob/main/docs/HARDWARE.md).

| | |
|---|---|
| **SoC** | Espressif ESP32-P4 (dual-core RISC-V @ 360 MHz, with single-precision FPU) |
| **RAM** | 768 KB on-chip + 32 MB PSRAM (8-bit Octal SPI) |
| **Flash** | 16 MB SPI NOR (dual OTA partitions: 3 MB + 3 MB) |
| **Display** | 5-inch TFT, 720×1280 portrait, MIPI-DSI 1-lane, 60 Hz |
| **Touch** | GT911 capacitive multitouch |
| **Audio out** | ES8388 stereo codec, 3 W mono speaker (rear) |
| **Audio in** | ES7210 quad-mic TDM array (front-facing) |
| **Camera** | SC202CS 2 MP CMOS, 1-lane MIPI-CSI, RAW8 → ISP → RGB565 1280×720 @ 30 fps |
| **IMU** | BMI270 6-axis (accel + gyro) over I2C |
| **Wireless** | Wi-Fi 6 (2.4 GHz only) + BLE 5 |
| **USB** | USB-C, USB 2.0 host/device |
| **Storage** | microSD slot, FAT32, SDMMC 4-bit |
| **Battery** | 1500 mAh LiPo, ~3 h runtime, charges to 80% in ~45 min |
| **Power** | 5 V via USB-C, charge LED on rear |
| **Expansion** | Module13.2 stack connector (top), Grove Port A I2C (side) |
| **Dimensions** | 130 mm × 80 mm × 17 mm |
| **Weight** | ~210 g |

## What's *not* on board

Worth knowing what isn't here, so you don't go looking:

- **No Ethernet.** Wi-Fi only.
- **No Bluetooth audio profile.** BLE is for low-energy data only — not A2DP / HFP.
- **No headphone jack.** Audio out is the speaker only.
- **No magnetic charging.** USB-C wired only.
- **No proximity sensor.** The IMU is for orientation; there's no IR proximity.
- **No HDMI / video out.** The screen is the screen.
- **No fingerprint reader.**
- **No GPS.**

If you want any of those, you're looking at expansion via Grove I2C or the Module13.2 stack. See [Grove Port A](/docs/hardware/grove-port-a).

## Memory budget

The 768 KB internal SRAM is the precious resource. Most of TinkerTab's working memory lives in PSRAM:

- **LVGL pool** — 64 KB BSS base + 2 MB PSRAM expansion (live)
- **Voice WS task stack** — 8 KB internal SRAM
- **Mic task stack** — 32 KB **PSRAM-backed** (bumped from 16 KB after the Wave 19 OPUS bisect)
- **Camera framebuffer** — PSRAM
- **Media cache** — 5-slot LRU, ~2.9 MB PSRAM total

The internal SRAM is mostly framework + driver stacks. See the [stability guide](/docs/firmware-reference/stability-guide) for the BSS-static rules.
