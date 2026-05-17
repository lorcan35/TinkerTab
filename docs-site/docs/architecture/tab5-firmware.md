---
title: Tab5 firmware overview
sidebar_label: Tab5 firmware overview
---

# Tab5 firmware overview

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

The TinkerTab firmware runs on the M5Stack Tab5's ESP32-P4. It is a **thin client** — every piece of intelligence (STT, LLM, TTS, conversation context, skills, scheduler, memory) lives on the Dragon Q6A server. Tab5 owns the display, the microphones, the speaker, the camera, the touch panel, the SD card, the Wi-Fi, and the NVS settings. It sends audio + text + images to Dragon over a single WebSocket and renders the responses.

## Stack

- **ESP-IDF 5.5.2** pinned via `dependencies.lock`
- **FreeRTOS** (the IDF default kernel)
- **LVGL 9.2.2** for UI — partial render mode, two 144 KB draw buffers in PSRAM
- **esp_video** + `esp_cam_sensor` for the SC202CS camera (V4L2-style)
- **esp_codec_dev** for ES8388 (DAC) + ES7210 (quad mic)
- **WebSocket transport** via `esp_websocket_client`

## The service-registry pattern

`main/service_registry.{c,h}` defines a small lifecycle abstraction. Each service has `init() → start() → stop() → deinit()` plus a state machine. Services declared at boot:

| Service | Owns |
|---------|------|
| `service_audio` | I2S init, ES8388 + ES7210 codec setup |
| `service_display` | LVGL + DPI panel bring-up |
| `service_dragon` | Voice WS client, mDNS lookup, touch-relay lifecycle |
| `service_network` | Wi-Fi STA, reconnect logic |
| `service_storage` | SD card mount, NVS init |

Boot flow in [`main/main.c`](https://github.com/lorcan35/TinkerTab/blob/main/main/main.c): hardware init → service bring-up → LVGL → watchdog.

## The single voice WebSocket

[`main/voice.c`](https://github.com/lorcan35/TinkerTab/blob/main/main/voice.c) holds the entire Dragon link. One persistent WS to port 3502. Mic capture → 16 kHz PCM → WS uplink. Replies come back as JSON event frames + binary TTS chunks.

Five voice modes (`vmode` NVS): Local, Hybrid, Cloud, TinkerClaw, Onboard (K144). Modes 0–3 talk to Dragon over WS; mode 4 routes via UART to the stacked K144 module.

## UI overlays vs screens

Most everyday flows happen on the home screen with **fullscreen overlays**:

- Settings / Chat / Voice are LVGL overlays on the home screen, hidden/shown not destroyed/created.
- Notes / Files / Camera / Memory / Sessions / Agents are separate screens loaded via `lv_screen_load_anim()`.

The hide/show pattern is intentional. Internal SRAM fragments under repeated create/destroy cycles; hide/show keeps allocations stable. See the [stability guide](/docs/firmware-reference/stability-guide).

## Memory layout

- **Internal SRAM** (~512 KB) — task stacks, LVGL BSS pool (64 KB), driver framebuffers
- **PSRAM** (32 MB) — LVGL expansion pool (2 MB), camera framebuffer, mic-task stack (32 KB), media cache (~2.9 MB)

Big buffers (>4 KB) MUST go in PSRAM. Static BSS arrays eat the limited internal SRAM and crash boot.

## What it doesn't do

- **No AI inference.** Even when "Onboard" mode is selected, the inference happens on the K144 module — Tab5 just brokers the UART transaction.
- **No conversation history.** Every turn ships the message to Dragon, which writes to MessageStore. Tab5 caches the recent few in PSRAM for the chat overlay; that cache is volatile.
- **No skill execution.** Skills are Python files on Dragon. Tab5 only renders typed widget state.
- **No web stack.** The CDP browser-streaming path that Tab5 originally shipped with was retired in PR #155 — voice-first is the product.

## Where to look next

- [Voice pipeline](/docs/architecture/voice-pipeline) — how a turn flows from mic to speaker
- [WS protocol](/docs/firmware-reference/ws-protocol) — the actual wire format
- [Stability guide](/docs/firmware-reference/stability-guide) — the rules that keep this thing booting
- [Build & flash](/docs/firmware-reference/build-flash) — get a binary on the device
