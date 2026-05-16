---
title: What's in the box
sidebar_label: What's in the box
---

# What's in the box

A working TinkerTab deployment is three pieces of hardware. The first two are required; the third is optional but unlocks offline inference.

## 1. M5Stack Tab5 (required)

The face.

- ESP32-P4 dual-core RISC-V at 360 MHz
- 32 MB PSRAM, 16 MB flash
- 5-inch portrait DSI display, 720×1280, capacitive multitouch
- 4-mic TDM array (ES7210) + 3 W speaker (ES8388)
- 2 MP MIPI-CSI camera (SC202CS)
- Wi-Fi 6 + BLE 5
- USB-C, microSD slot, 1500 mAh battery
- Grove Port A (I2C) + Module13.2 stack connector

Buy it: [M5Stack Tab5](https://shop.m5stack.com/products/m5stack-tab5-iot-development-kit). Flash it with [TinkerOS](https://github.com/lorcan35/TinkerTab) per [Build & flash](/docs/firmware-reference/build-flash).

## 2. Dragon Q6A (required)

The brain.

- Radxa Dragon Q6A — Qualcomm QCS6490, 12 GB RAM, 12 TOPS NPU
- Ubuntu 22.04 / 24.04 ARM64
- 12 V power, gigabit ethernet, optional Wi-Fi (typically disabled)
- ~$300 SBC; *any* modern Linux box with 12 GB+ RAM works as a substitute

The Dragon hosts:

- **Voice server** on port 3502 (WebSocket + REST)
- **Dashboard** on port 3500 (web UI)
- **Ollama** on port 11434 (local LLM inference)
- **SearXNG** on port 8888 (self-hosted web search)
- **Optional ngrok tunnels** for remote access

Setup: [Install Dragon](/docs/dragon/install).

## 3. M5Stack LLM Module Kit "K144" (optional)

The escape hatch when Dragon is unreachable. Stack it on top of Tab5 with the **Module13.2 LLM Mate** carrier between them:

```
Tab5 base → Mate (USB-C powered) → K144
```

The K144 hosts an Axera AX630C NPU running `qwen2.5-0.5B` at ~8 tok/s and a `sherpa-ncnn` ASR pipeline. When Tab5 can't reach Dragon (Wi-Fi outage, Dragon rebooting), it routes the next text turn through the K144 over UART instead. You see a small "Using onboard LLM" toast on screen and conversation continues.

[More on K144 →](/docs/hardware/k144)

## Optional add-ons

| Hardware | What it adds | How |
|----------|--------------|-----|
| Grove BME280 | Temp + humidity in LLM context | Plug into Port A — `apt install` nothing, the firmware probes I2C at boot |
| Grove SHT31 | Temp + humidity (alternate) | Same |
| Grove VL53L0X | Distance / occupancy | Same |
| Tripod / mount | Hands-free placement | M5Stack's official mount, or any 1/4"–20 thread |

## What you do *not* need

- A microphone — the Tab5 has 4 of them.
- A keyboard — the on-screen keyboard handles text input. SSH into Dragon for server admin.
- Cloud subscriptions — everything runs locally by default. Hybrid + Full Cloud modes need an OpenRouter API key, but they're opt-in.
- A wake word — there's a push-to-talk orb. (See the [glossary's wake-word entry](/docs/glossary) for why.)
