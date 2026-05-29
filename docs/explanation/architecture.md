---
audience: developer
type: explanation
prerequisites: [how-the-stack-fits-together.md](how-the-stack-fits-together.md)
last-verified: 2026-05-29
---
# TinkerTab architecture — how the firmware is built and why

## The question

You are about to read or change the [Tab5](../../GLOSSARY.md) firmware and you
want the mental model first: what runs on the device, what runs on the
[Dragon](../../GLOSSARY.md), how the firmware is organized internally, and why
the boundaries fall where they do. This page draws that model.

It is the firmware-side companion to
[how the stack fits together](how-the-stack-fits-together.md), which maps the
four repos at the product level. That page answers "how do Tab5, Dragon, and
TinkerON relate?"; this one answers "how is the *TinkerTab* firmware itself put
together, and why?" It does not give you a recipe — for building and flashing
see [the dev-setup how-to](../how-to/dev-setup.md) and
[flash the firmware](../how-to/flash-firmware.md).

## The model

### Thin face, fat brain

TinkerTab is the firmware for the M5Stack Tab5: an ESP32-P4 with a 720×1280 MIPI
DSI display, a 4-mic TDM array, an ES8388 speaker, an SC202CS camera, and Wi-Fi
through a hosted ESP32-C6. It is deliberately a **thin client**. It owns the
hardware and the native [LVGL](../../GLOSSARY.md) UI; it owns no intelligence.

```
        +-----------------------------+                 +-------------------------------+
        |  Tab5  (TinkerTab — this repo)|  one WebSocket  |  Dragon Q6A  (TinkerBox)      |
        |  "the face" — thin client    | <==============> |  "the brain" — all intelligence|
        |                              |  ws://host:3502  |                               |
        |  - LVGL v9 UI                |  /ws/voice       |  - STT (Moonshine)            |
        |  - 4-mic array + ES8388 spkr |   audio frames > |  - LLM (llama-server / cloud) |
        |  - SC202CS camera, touch     | < STT/LLM/TTS    |  - TTS (Piper / Kokoro)       |
        |  - SD card, NVS settings     |   config/events  |  - embeddings + memory + RAG  |
        |  - ESP32-P4 + ESP32-C6 Wi-Fi |                  |  - sessions, REST API, dashboard|
        +-----------------------------+                  +-------------------------------+
```

The split is sharp and the [`CLAUDE.md`](../../CLAUDE.md) "Repo Separation"
section states it as a rule:

- **TinkerTab owns** the LVGL UI, mic/speaker/camera/touch, SD card, Wi-Fi, and
  [NVS](../../GLOSSARY.md) settings. It *sends* audio frames, text turns, and
  device registration; it *receives* STT results, LLM responses, TTS audio, and
  config updates. Storage on the device is the SD card (audio recordings,
  offline queue) and NVS (settings). **There is no database on the Tab5.**
- **TinkerBox (the Dragon)** owns STT, the LLM, TTS, embeddings, sessions, the
  conversation engine, the REST API, the dashboard, and the database. All
  intelligence is here.

The wire contract between the two lives in TinkerBox's
[`docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md);
Tab5 implements the client side. The cross-stack picture is in TinkerBox's
[`docs/ARCHITECTURE.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/ARCHITECTURE.md).

### The four firmware layers

Inside the firmware, the code is organized into four layers, from hardware up to
UI. The `main/` directory holds all of them.

```
+--------------------------------------------------------------+
|  UI layer — LVGL v9                                          |
|  ui_core, ui_home, ui_voice, ui_chat, ui_settings, ...       |
|  one orb across home + voice; overlays hidden, not destroyed |
+--------------------------------------------------------------+
|  Dragon link — the one WebSocket                            |
|  voice.c (state machine + mic/playback tasks)                |
|  voice_ws_proto.c (frame routing)  voice_modes.c (6 modes)   |
+--------------------------------------------------------------+
|  Coordination — what runs when                              |
|  mode_manager (IDLE <-> VOICE)   service_registry (lifecycle)|
+--------------------------------------------------------------+
|  Hardware drivers                                           |
|  audio (ES8388), mic (ES7210), camera (SC202CS), imu, rtc,   |
|  battery, wifi (C6 via SDIO), sdcard, touch (GT911), display |
+--------------------------------------------------------------+
```

### The service registry — one lifecycle for every subsystem

Every heavy subsystem is wrapped in a uniform service lifecycle so the firmware
can bring it up, tear it down, and recover it through one interface rather than
ad-hoc init code scattered across `main.c`. The state machine is:

```
NONE -> INITIALIZED -> RUNNING -> STOPPED -> RUNNING -> ...
                                     \-> ERROR (needs recovery)
```

There are five services, defined in `main/service_registry.{c,h}` with one
implementation file each:

| Service | File | Owns |
|---|---|---|
| `STORAGE` | `service_storage.c` | SD card (SDMMC) + NVS |
| `DISPLAY` | `service_display.c` | LVGL + DSI panel bring-up |
| `AUDIO` | `service_audio.c` | I2S, ES8388 DAC, ES7210 mic |
| `NETWORK` | `service_network.c` | Wi-Fi (STA, connect, reconnect) |
| `DRAGON` | `service_dragon.c` | the voice WebSocket lifecycle |

Each service exposes `init` / `start` / `stop` so the boot path and the mode
manager can sequence them deterministically instead of inferring init order from
call sites. This is the pattern `CLAUDE.md` calls out as "better than Mooncake
for embedded" — it is the firmware's spine.

### The mode manager — one heavy subsystem at a time

The ESP32-P4 shares roughly 512 KB of internal SRAM between FreeRTOS and the DMA
pool, and three subsystems contend for that DMA RAM: SDIO Wi-Fi, I2S audio, and
MIPI DSI video. Run them all at once and allocations fail. The **mode manager**
(`main/mode_manager.{c,h}`) enforces that only one heavy subsystem runs at a
time. The device is always in exactly one mode:

| Mode | Active services | Purpose |
|---|---|---|
| `IDLE` | Wi-Fi | No streaming, no voice |
| `STREAMING` | Wi-Fi + MJPEG + Touch WS | Legacy desktop streaming (retired path) |
| `VOICE` | Wi-Fi + Voice WS + Mic + Speaker | Voice assistant |
| `BROWSING` | Wi-Fi + MJPEG + Touch WS | Reserved (same as STREAMING) |

Mode switches are mutex-protected and idempotent: the manager stops the old
mode's services, waits for DMA to settle, then starts the new mode's services.
In practice today the firmware lives in `IDLE` and `VOICE` — the CDP
browser-streaming path that drove `STREAMING` was retired (see "Why it's built
this way" below), so `mode_manager` is now a thin mutex wrapper around
`voice_connect` / `voice_disconnect`, kept because `ui_voice`, `ui_notes`, and
`service_dragon` flip modes from several tasks.

### The Dragon link — one WebSocket, many message types

The Tab5 holds exactly one persistent [WebSocket](../../GLOSSARY.md) to the
Dragon at `ws://<host>:3502/ws/voice`, and *all* traffic multiplexes over it:
voice, text, vision, video, config, and observability. Three files carry it:

- **`voice.c`** — the public voice API, the voice state machine
  (`IDLE`/`CONNECTING`/`READY`/`LISTENING`/`PROCESSING`/`SPEAKING`/`RECONNECTING`),
  the mic-capture task, the playback drain task, and the reconnect watchdog.
- **`voice_ws_proto.c`** — the frame-routing layer: the JSON RX dispatcher, the
  binary magic-tag dispatcher (`VID0` video / `AUD0` call audio / untagged raw
  PCM → STT), the `esp_websocket_client` event callback, and the UI-async
  helpers.
- **`voice_modes.c`** — the six-tier voice-mode dispatcher that decides where a
  text turn is routed.

Untagged binary frames are raw 16 kHz mono PCM going straight into STT — that is
the legacy mic path and it stays magic-less for backward compatibility. Tagged
frames carry a 4-byte magic plus a big-endian length: `VID0` for JPEG video both
directions, `AUD0` for call-mode PCM.

### Where a voice turn runs — the six modes

The same orb tap or wakeword fires a turn, but *where the work happens* depends
on the active [voice mode](../../GLOSSARY.md):

| Mode | STT | LLM | TTS | Needs Dragon? |
|---|---|---|---|---|
| 0 — Local | Moonshine (Dragon) | Dragon-local | Piper (Dragon) | Yes |
| 1 — Hybrid | OpenRouter | Dragon-local | OpenRouter | Yes |
| 2 — Cloud | OpenRouter | OpenRouter | OpenRouter | Yes (audio pipe) |
| 3 — TinkerClaw | Dragon | [TinkerClaw](../../GLOSSARY.md) gateway | Dragon | Yes (audio pipe) |
| 4 — Onboard (K144) | K144 | K144 | K144 | **No** |
| 5 — Solo | OpenRouter | OpenRouter | OpenRouter | **No** |

Modes 0–3 are real on-the-wire `voice_mode` values. Modes 4 and 5 are
**Tab5-side-only**: they downconvert to `0` on the wire so the Dragon never sees
them, and the `config_update` ACK handler in `voice.c` filters out the Dragon's
echo so local NVS keeps the true 4/5 value. The active mode persists in the NVS
`vmode` key.

### The optional on-device module — TinkerON / K144

The K144 ([TinkerON](../../GLOSSARY.md)) is an M5Stack LLM Module Kit (AX630C NPU)
that stacks onto the Tab5 over the M5-Bus (UART or USB transport) through a
Module13.2 Mate carrier. When present it adds always-on "Hey Tinker" wakeword and
can run a whole voice turn — ASR, LLM, and TTS — entirely on-device with no
Dragon (voice mode 4). It is strictly additive: the firmware enforces six
modularity rules so the Tab5 **never depends** on it being present, gray-outs
the relevant UI when it is absent, and degrades gracefully on hot-unplug. The
K144 surface lives in `voice_m5_llm.c`, `voice_onboard.c`, and
`voice_wakeword.c`, and talks the [StackFlow](../../GLOSSARY.md) JSON protocol
over the M5-Bus.

### The boot sequence

`main/main.c` brings the layers up bottom-first so each layer can rely on the
one below it:

1. Platform init — I2C bus, IO expanders, Wi-Fi power
2. Service init — allocate and configure hardware for all five services
3. Peripheral drivers — camera, IMU, RTC, battery
4. Service start — Storage → Display → Audio → Network
5. Dragon link start (needs Wi-Fi)
6. LVGL init and splash screen
7. Home screen transition with deferred overlay creation
8. Debug HTTP server start (port 8080)
9. NTP time sync (best-effort)
10. Serial command loop

The voice WebSocket connects silently at boot and the device registers
immediately, so the first mic tap does not pay a 5–15 s connect cost.

### Observing the running system

The firmware is built to be driven and inspected from a workstation. The
[debug server](../reference/debug-server.md) on port 8080 exposes screenshots,
touch injection, navigation, voice control, and K144 diagnostics. Internally,
`tab5_debug_obs_event(kind, detail)` writes to a 256-entry ring read via
`GET /events?since=N`, which the Python end-to-end harness in `tests/e2e/` polls
to drive long user-story flows. This observability surface is part of the
architecture, not an afterthought — it is how the device is tested without a
human watching the screen.

## Why it's built this way

- **Thin client, fat brain.** The ESP32-P4's ~512 KB of tight internal SRAM
  cannot host a useful LLM. Pushing all intelligence to the Dragon keeps the
  firmware small, lets the brain upgrade independently (a new model is a Dragon
  deploy, not a reflash), and lets one Dragon serve a fleet of faces. The cost is
  a hard dependency on the Dragon for modes 0–3 — which is exactly why the
  optional on-device fallback (mode 4) and the direct-cloud path (mode 5) exist.

- **The service registry over ad-hoc init.** Five subsystems with hardware that
  can fail and need recovery is enough complexity to justify a uniform
  lifecycle. The alternative — init code threaded through `main.c` with implicit
  ordering — made recovery and mode switching error-prone. The explicit
  `NONE → INITIALIZED → RUNNING → STOPPED → ERROR` state machine gives the mode
  manager a clean contract to sequence against. `CLAUDE.md` records the decision
  not to adopt M5Stack's Mooncake here: the service registry is the better fit
  for this embedded target.

- **One heavy subsystem at a time.** SDIO Wi-Fi, I2S audio, and MIPI DSI all draw
  from the same scarce internal DMA RAM. Without the mode manager's
  stop-old → settle → start-new discipline, switching between video streaming and
  voice exhausted the DMA pool and crashed. The `heap_watchdog.c` safety net
  catches the residual case: when `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` free
  drops below 16 KB for two consecutive 60 s samples, it reboots (honoring a
  voice-active grace window) so a radio-silent device recovers without a
  serial reflash.

- **One WebSocket, many message types.** Multiplexing voice, text, vision, video,
  config, and events over a single persistent connection avoids connection churn
  on a memory-constrained device and keeps the protocol in one place. The binary
  magic-tag scheme (`VID0` / `AUD0` / untagged) lets one socket carry several
  binary payload kinds while keeping the legacy raw-PCM mic path unchanged.

- **Voice-first, not a remote display.** The Tab5 originally shipped a CDP
  browser-streaming path (MJPEG video + touch relay over port 3501). It was
  retired in PR #155 because the product is the voice assistant, not a remote
  screen. Doubling down on voice is what differentiates the Tab5 from every other
  IoT display. The `STREAMING`/`BROWSING` modes and their old driver files
  (`dragon_link`, `mjpeg_stream`, `udp_stream`, `touch_ws`, `mdns_discovery`) are
  gone; the mode enum retains the names for compatibility but the firmware reaches
  the Dragon only over the voice WebSocket.

- **Each repo stands alone.** TinkerTab is documented as its own front door and
  cross-links the other three repos as peers rather than nesting under a single
  umbrella. A reader who learns one repo's layout can navigate any of them, but
  each is fully self-contained — which is why the deep wire contract and the
  cross-stack map live in TinkerBox, and this repo links to them rather than
  copying them.

## See also

- [How the stack fits together](how-the-stack-fits-together.md) · [Getting started](../tutorials/getting-started.md)
- [Dev setup](../how-to/dev-setup.md) · [Flash the firmware](../how-to/flash-firmware.md) · [Deploy](../how-to/deploy.md)
- [Debug server reference](../reference/debug-server.md) · [GLOSSARY](../../GLOSSARY.md)
- System architecture (cross-stack): [TinkerBox `docs/ARCHITECTURE.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/ARCHITECTURE.md)
- Wire protocol: [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md)
