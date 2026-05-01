---
title: Glossary
sidebar_label: Glossary
---

# Glossary

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Quick lookups for jargon used across the project. Merged from TinkerTab + TinkerBox per-repo glossaries.

## A

**AEC** — Acoustic Echo Cancellation. Removes the speaker's playback from the mic signal so the device doesn't hear itself talk. Wired up but currently retired pending a custom WakeNet model + TDM-slot audit.

**AUD0** — Magic prefix on call-mode audio frames between Tab5 ↔ Dragon ↔ peer. `"AUD0"` (4 bytes) + 4-byte BE u32 length + raw 16 kHz mono int16 PCM.

**AX630C** — The Axera NPU on the K144 LLM module. ~3.2 TOPS. Runs `qwen2.5-0.5B` at ~8 tok/s.

## B

**BSP** — Board Support Package. The lower-level driver layer above ESP-IDF, in `bsp/tab5/`.

**BSS** — Block Started by Symbol — the section of a binary holding zero-initialized static data. Large BSS allocations on ESP32-P4 push the FreeRTOS timer task over its SRAM canary at boot. See [stability guide](/docs/firmware-reference/stability-guide).

## C

**Caddy** — The web server hosting this docs site.

**CDP** — Chrome DevTools Protocol. The retired browser-streaming path Tab5 originally shipped with.

**CSI** — Camera Serial Interface. MIPI-CSI is the protocol the SC202CS sensor uses to feed video to the ESP32-P4.

## D

**Dragon** — The Linux box running TinkerBox. Default deployment is a Radxa Dragon Q6A SBC; any 12 GB+ Linux machine works.

**DSI** — Display Serial Interface. MIPI-DSI is the protocol Tab5's 5-inch panel uses.

## E

**EXT5V_EN** — IO-expander pin (1, P4) that gates the 5 V rail to Grove Port A and other expansion connectors. Refcounted via `tab5_ext5v_acquire/release`.

**ESP32-CAM** — A *different* Espressif microcontroller, NOT a Grove sensor. Has Grove ports for plugging things into IT, but doesn't act as an I2C peripheral that Tab5 can read.

**ESP-IDF** — Espressif IoT Development Framework. Pinned to **5.5.2** via `dependencies.lock`.

## F

**FATFS** — The FAT32 filesystem driver Tab5 uses for the SD card. `CONFIG_FATFS_LFN_NONE=1` forces 8.3 short names — this is why video recordings save as `.MJP` not `.mjpeg`.

**Fleet** — A list of `ModelSpec` entries declared in Dragon's `config.yaml` when `llm.backend = "router"`. Each spec has caps + tier + priority. The router picks per-turn from the fleet.

## G

**Grove** — A standardized 4-pin I2C connector format. Tab5's Port A is a Grove-compatible HY2.0-4P jack on the side.

**GT911** — The capacitive multitouch controller on Tab5's display.

## H

**HTP** — Hexagon Tensor Processor. The Qualcomm NPU on the QCS6490 in Dragon Q6A. Used by the `npu_genie` LLM backend.

## I

**I2S** — A serial audio bus. Tab5's I2S_NUM_1 carries both TX (speaker) and RX (mic) in a mixed STD/TDM configuration.

## J

**JPEG engine** — ESP32-P4 has one hardware JPEG encoder. Mutex-guarded inside `voice_video.c` so the camera recording path and the call uplink don't collide.

## K

**K144** — The M5Stack LLM Module Kit. AX630C-based offline inference module. Stacks on Tab5 via the **Module13.2 Mate carrier**.

**KWS** — Keyword Spotting. The "Hey Tinker" feature. Currently retired; revival path uses sherpa-onnx open-vocabulary KWS on the K144.

## L

**LVGL** — Light and Versatile Graphics Library. Version 9.2.2 in this firmware.

## M

**MCAUSE** — RISC-V machine-cause register. **MCAUSE 0x1b** on ESP32-P4 = stack protection fault (per-task canary tripped).

**MessageStore** — Dragon's append-only conversation history. `dragon_voice/messages.py`.

**Mate** — The Module13.2 LLM Mate carrier. Required for stacking the K144 — direct K144-on-Tab5 stack causes M5-Bus 5 V collisions that wedge the AX630C silicon.

**Module13.2** — M5Stack's stack-on connector standard. Tab5's top-mount expansion port.

## N

**Ngrok** — Tunneling service used for `tinkerclaw-{voice,dashboard,gateway}.ngrok.dev` public endpoints.

**NPU** — Neural Processing Unit. Tab5 doesn't have one; Dragon Q6A's Hexagon HTP and the K144's AX630C both do.

**NVS** — Non-Volatile Storage. The ESP-IDF flash-backed key-value store for settings.

## O

**Onboard mode** — Voice mode 4. Routes every text turn through the K144 module instead of Dragon.

**OPUS** — Audio codec, 24 kbps. Encoder uplink path is enabled as of Wave 19. Decoder ready for Dragon → Tab5 OPUS TTS.

## P

**Piper** — Local TTS via ONNX. Default in voice modes 0/3.

**PSRAM** — Pseudo-Static RAM. The 32 MB external memory on Tab5. Big buffers must go here.

**Push-to-talk** — Tap-and-hold-the-orb voice trigger. We don't have a wake word.

## Q

**QCS6490** — Qualcomm SoC powering the Radxa Dragon Q6A. ARM64 + Hexagon HTP + Adreno GPU.

**Q6A** — The Radxa Dragon Q6A SBC.

## R

**Router** — Dragon's `CapabilityAwareRouter`. Picks an LLM per-turn from the declared fleet based on required modalities + active voice-mode tier.

## S

**SC202CS** — The 2 MP MIPI-CSI camera sensor on Tab5. SCCB I2C address 0x36 (NOT 0x30, despite some schematics).

**SDIO** — A protocol that runs on the SD-card slot. Tab5's Wi-Fi 6 chip is reachable via SDIO.

**SearXNG** — Self-hosted metasearch engine on Dragon port 8888. Backs the `web_search` tool.

**Sherpa-onnx** — ASR + KWS toolkit shipped on the K144. The streaming Zipformer model is what does on-device ASR.

**SILK** — The lower-band component of the OPUS codec (CELT is the upper-band). NSQ = Noise Shaping Quantizer. Wave 19 root-caused a stack overflow there.

**Skill** — A Python file in `dragon_voice/tools/` that the LLM can call. See [Skill SDK](/docs/dragon-reference/skill-sdk).

**SRAM** — Static RAM. Tab5's internal SRAM (~512 KB) is the precious resource; PSRAM (32 MB) is plentiful.

**StackFlow** — The K144's onboard JSON-over-TCP service. Verbs include `sys.ping`, `sys.hwinfo`, `sys.lsmode`, `sys.reset`, `sys.reboot`, `sys.version`.

**STT** — Speech-to-Text. Default backend is Moonshine.

## T

**Tab5** — The 5-inch ESP32-P4 device this firmware runs on.

**TDM** — Time-Division Multiplexed. The mode the ES7210 quad-mic uses — 4 slots in a single I2S frame.

**TinkerClaw** — Either: (1) the OpenClaw fork running as a sidecar on Dragon (`vmode=3`), or (2) the umbrella name for the Tab5 + Dragon platform. Disambiguate by context.

**TJPGD** — Tiny JPEG Decoder. The library Tab5 uses to decode inbound JPEG frames (chat media, video calls).

**TLSF** — Two-Level Segregated Fit. The allocator backing LVGL's memory pool.

**TTS** — Text-to-Speech. Default backend is Piper.

## U

**UART** — Universal Asynchronous Receiver/Transmitter. The serial protocol Tab5 uses to talk to the K144 (Port C UART, 115200 8N1, auto-bumps to 1.5 Mbps).

## V

**VAD** — Voice Activity Detection. Dragon's pipeline uses silero VAD plus an adaptive RMS gate for dictation.

**VID0** — Magic prefix on video call frames. `"VID0"` + 4-byte BE u32 length + JPEG bytes.

**vmode** — NVS key for the active voice mode. 0 = Local, 1 = Hybrid, 2 = Cloud, 3 = TinkerClaw, 4 = Onboard (K144).

## W

**Wake word** — A short phrase the device listens for ("Hey Siri" style). TinkerTab does NOT have one in shipped firmware. Push-to-talk only.

**Watcher (SenseCAP)** — Seeed Studio's voice device. Inspired the visual layout of this docs site.

**Widget** — A typed state object that Dragon emits and Tab5 renders. Six types: live, card, list, chart, media, prompt.

**WS** — WebSocket. The single transport between Tab5 and Dragon (port 3502).

## X

**xLAM** — A function-calling-trained LLM family. Tested in the local LLM gauntlet; fast tool-picker, weak responder. Drives the `dual` backend.

**xTimerCreate** — FreeRTOS timer service API. Don't call from boot — uses the timer service task whose stack alloc can fail under boot pressure. Use `esp_timer` instead.
