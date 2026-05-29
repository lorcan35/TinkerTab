---
audience: developer
type: explanation
prerequisites: none
last-verified: 2026-05-29
---
# How the stack fits together — Tab5, Dragon, and TinkerON

## The question

You have heard of [TinkerTab](../../GLOSSARY.md), [TinkerBox](../../GLOSSARY.md),
the [Dragon](../../GLOSSARY.md), and [TinkerON](../../GLOSSARY.md), and you want
a mental model of **how they relate** before diving into any one repo. This page
draws the map. It does not teach you how to do anything — for that, start with
the [getting-started tutorial](../tutorials/getting-started.md).

## The model

The live product is two devices talking over **one WebSocket**, with an optional
on-device LLM module hanging off the Tab5:

```
        +-----------------------------+                 +-------------------------------+
        |  Tab5  (TinkerTab firmware)  |   one WebSocket  |  Dragon Q6A  (TinkerBox)      |
        |  "the face" — thin client    | <==============> |  "the brain" — all intelligence|
        |                              |  ws://host:3502  |                               |
        |  - LVGL v9 UI                |  /ws/voice       |  - STT (Moonshine)            |
        |  - 4-mic array + ES8388 spkr |                  |  - LLM (llama-server / cloud) |
        |  - SC202CS camera, touch     |   audio frames > |  - TTS (Piper / Kokoro)       |
        |  - SD card, NVS settings     | < STT/LLM/TTS    |  - embeddings + memory + RAG  |
        |  - ESP32-P4 + ESP32-C6 Wi-Fi |   config/events  |  - sessions, REST API, dashboard|
        +--------------+---------------+                  +-------------------------------+
                       |
              M5-Bus UART / USB
                       |
        +--------------v---------------+
        |  K144 / TinkerON module       |
        |  (AX630C NPU) — optional      |
        |  - on-device ASR (sherpa-ncnn)|
        |  - on-device LLM + TTS        |
        |  - always-on wakeword         |
        +-------------------------------+
```

- **Tab5 (TinkerTab)** is a deliberately *thin client*. It owns hardware I/O and
  the native UI but no intelligence. It captures microphone audio, streams it to
  the Dragon, and plays back the audio that streams down.
- **Dragon (TinkerBox)** is the *brain*. Speech-to-text, the LLM, text-to-speech,
  embeddings, memory, sessions, the dashboard, and OTA all live here. The wire
  contract between the two is documented in TinkerBox's `docs/protocol.md`; the
  cross-stack architecture is in TinkerBox's `docs/ARCHITECTURE.md`.
- **K144 / TinkerON** is an *optional* on-device module. When present it adds
  always-on "Hey Tinker" wakeword and can run a whole voice turn — ASR, LLM, and
  TTS — entirely on-device with no Dragon. The Tab5 must never *depend* on it
  being present.
- **[PingOS](../../GLOSSARY.md)** is a portable, BSP-abstracted layer derived
  from the Tab5 UI so the interface can run on hardware beyond the M5Stack Tab5.
  **[TinkerClaw](../../GLOSSARY.md)** is an optional agent sidecar (voice mode 3)
  that runs its own LLM + tools + browser automation on the Dragon.

### Where a voice turn can run — the six modes

The same orb-tap or wakeword fires a turn, but *where the work happens* depends
on the active [voice mode](../../GLOSSARY.md):

| Mode | STT | LLM | TTS | Needs Dragon? |
|---|---|---|---|---|
| 0 — Local | Moonshine (Dragon) | Dragon-local | Piper (Dragon) | Yes |
| 1 — Hybrid | OpenRouter | Dragon-local | OpenRouter | Yes |
| 2 — Cloud | OpenRouter | OpenRouter | OpenRouter | Yes (as audio pipe) |
| 3 — TinkerClaw | Dragon | TinkerClaw gateway | Dragon | Yes (as audio pipe) |
| 4 — Onboard (K144) | K144 | K144 | K144 | **No** |
| 5 — Solo | OpenRouter | OpenRouter | OpenRouter | **No** |

Modes 4 and 5 are Tab5-side-only: they downconvert to `0` on the wire so the
Dragon never sees them.

## Why it's built this way

- **Thin client, fat brain.** The ESP32-P4 has ~512 KB of tight internal SRAM;
  it cannot host a useful LLM. Pushing all intelligence to the Dragon keeps the
  firmware small, lets the brain upgrade independently (new models are a Dragon
  deploy, not a reflash), and means a single Dragon can serve a fleet of faces.
- **One WebSocket, many message types.** Multiplexing voice, text, vision,
  video, config, and events over a single persistent connection avoids
  connection churn on a memory-constrained device and keeps the protocol in one
  place (TinkerBox `docs/protocol.md`).
- **Each repo stands alone.** Rather than a single umbrella front door, the four
  repos cross-link as peers (the "Part of the Tinker stack" block). A reader who
  learns one repo's layout can navigate any of them, but each repo is fully
  documented on its own.
- **Optional on-device fallback.** TinkerON exists so the Tab5 can keep working
  when the Dragon is unreachable (and for hands-free wakeword), but it is strictly
  additive — the Tab5 degrades gracefully when it is absent.

## See also

- [Getting started](../tutorials/getting-started.md) · [How to flash the firmware](../how-to/flash-firmware.md)
- [Debug server reference](../reference/debug-server.md) · [GLOSSARY](../../GLOSSARY.md)
- System architecture (cross-stack): [TinkerBox `docs/ARCHITECTURE.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/ARCHITECTURE.md)
- Wire protocol: [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md)
