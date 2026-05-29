---
audience: developer
type: explanation
prerequisites: none
last-verified: 2026-05-29
---
# The Tinker stack — how the pieces fit

## The question

You are a new contributor staring at four repositories and you want to know,
before you read anything else: **what are these projects, who owns what, how does
data flow between them, and where do I go for the next level of detail?** This is
the map you read first. It does not teach you to do anything — for that, start
with the [getting-started tutorial](../tutorials/getting-started.md). It also
does not re-derive the architecture, the wire protocol, or the boot sequence;
those have their own pages and this one links to them.

There are two related explanation pages and it is worth knowing which to read
when:

- **This page** is the contributor's whole-system orientation: the four repos,
  the ownership boundaries, and the doc map.
- [How the stack fits together](how-the-stack-fits-together.md) is the shorter
  product-level "how do these relate?" answer.
- [TinkerTab architecture](architecture.md) goes one level deeper into *this*
  repo's firmware internals.

## The model

The live product is two devices talking over **one WebSocket**, with an optional
on-device LLM module hanging off the [Tab5](../../GLOSSARY.md), and two more
repos that are part of the family but are not on the voice-turn hot path.

```
                       THE LIVE PRODUCT (two boxes, one socket)

   +-------------------------------+                      +---------------------------------+
   |  Tab5  (TinkerTab firmware)    |    one WebSocket     |  Dragon Q6A  (TinkerBox)        |
   |  "THE FACE" — thin client      | <==================> |  "THE BRAIN" — all intelligence |
   |                                |   ws://host:3502     |                                 |
   |  - LVGL v9 UI, touch           |   /ws/voice          |  - STT (Moonshine)              |
   |  - 4-mic array, ES8388 spkr    |  audio frames  ===>  |  - LLM (llama-server / cloud)   |
   |  - SC202CS camera              |  text / config ===>  |  - TTS (Piper / Kokoro)         |
   |  - SD card, NVS settings       |  <=== STT/LLM/TTS    |  - embeddings, memory, RAG      |
   |  - ESP32-P4 + ESP32-C6 Wi-Fi   |  <=== events/config  |  - sessions, REST API, dashboard|
   +---------------+----------------+                      +----------------+----------------+
                   |                                                        |
          M5-Bus UART / USB                                       voice mode 3 (localhost)
                   |                                                        |
   +---------------v----------------+                      +----------------v----------------+
   |  K144 / TinkerON module        |                      |  TinkerClaw gateway  :18789     |
   |  (AX630C NPU) — OPTIONAL        |                      |  agent sidecar — OPTIONAL       |
   |  - on-device ASR (sherpa-ncnn) |                      |  - own LLM choice + tools       |
   |  - on-device LLM + TTS         |                      |  - browser automation           |
   |  - always-on "Hey Tinker"      |                      |  (Dragon becomes an audio pipe) |
   +--------------------------------+                      +---------------------------------+


   PingOS  — a SEPARATE product.  Turn any website into a REST API via a Chrome
             MV3 bridge + ahead-of-time compilation.  Not on the voice-turn path;
             shares the stack's docs conventions and peer-links as family.
```

### Who owns what

The ownership boundary between the face and the brain is the single most
important rule in the stack, and both `CLAUDE.md` files state it as law. Code
that crosses the line is a bug.

| Concern | Lives in | Repo |
|---|---|---|
| LVGL UI, screens, the orb | Tab5 | [TinkerTab](https://github.com/lorcan35/TinkerTab) |
| Mic / speaker / camera / touch / IMU / SD / Wi-Fi | Tab5 | TinkerTab |
| Device settings ([NVS](../../GLOSSARY.md)) | Tab5 | TinkerTab |
| Speech-to-text (STT) | Dragon | [TinkerBox](https://github.com/lorcan35/TinkerBox) |
| LLM inference + the multi-model router | Dragon | TinkerBox |
| Text-to-speech (TTS) | Dragon | TinkerBox |
| Embeddings, memory, RAG, sessions | Dragon | TinkerBox |
| Conversation engine, REST API, dashboard, database | Dragon | TinkerBox |
| The WebSocket wire contract | Dragon (spec) | TinkerBox `docs/protocol.md` |
| On-device wakeword + on-device voice turn | K144 module | TinkerTab (`voice_m5_llm.c`, `voice_onboard.c`, `voice_wakeword.c`) |
| Agent loop + tool execution + browser (voice mode 3) | TinkerClaw gateway | [TinkerClaw](https://github.com/lorcan35/TinkerClaw) |
| Website-to-API automation | PingOS | [PingOS](https://github.com/lorcan35/PingOS) |

The blunt version, straight from the repo separation rules:

- **Tab5 (TinkerTab)** is a deliberately *thin client*. It owns hardware I/O and
  the native UI but **no intelligence and no database**. It sends audio frames,
  text turns, and device registration; it receives STT results, LLM responses,
  TTS audio, and config updates. On-device storage is the SD card (recordings,
  offline queue) and NVS (settings) only.
- **Dragon (TinkerBox)** is the *brain*. STT, the LLM, TTS, embeddings, memory,
  sessions, the dashboard, OTA, and the database all live here. The brain can
  upgrade independently of the firmware — a new model is a Dragon deploy, not a
  reflash, and one Dragon can serve a fleet of faces.
- **K144 / TinkerON** is an *optional* module stacked onto the Tab5. When present
  it adds always-on "Hey Tinker" wakeword and can run an entire voice turn — ASR,
  LLM, TTS — on-device with no Dragon. The firmware enforces six modularity rules
  so the Tab5 **never depends** on it being present.
- **TinkerClaw** is the *optional* agent sidecar (OpenClaw-derived) on the Dragon
  at `localhost:18789`. Voice mode 3 routes the turn through it; in that mode the
  Dragon's own conversation engine, tools, and memory are bypassed and the Dragon
  becomes an audio pipe.
- **PingOS** is a *separate product*: a gateway that turns any website into a
  REST API through a Chrome MV3 extension bridge and ahead-of-time compilation.
  It is part of the Tinker family and shares these docs conventions, but it is
  not on the voice-turn hot path — treat its README as the front door.

### A voice turn, end to end

The default path (voice mode 0) shows the data flow:

```
  you speak
     │
     ▼
  Tab5 mic (ES7210, 48 kHz, 4-ch TDM) ─ slot 0 ─ downsample 3:1 ─► 16 kHz mono PCM
     │
     ▼  binary frames over the one WebSocket
  Dragon: STT (Moonshine) ─► transcript ─► LLM (llama-server) ─► reply text ─► TTS (Piper)
     │
     ▼  16 kHz PCM frames back down the same WebSocket
  Tab5 playback ─ upsample 1:3 ─► 48 kHz ─► ES8388 DAC ─► speaker
     │
     ▼
  you hear the answer
```

The same orb tap or wakeword fires every turn, but *where the work happens*
depends on the active [voice mode](../../GLOSSARY.md):

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
them. The full table is in the [voice-modes reference](../reference/voice-modes.md).

### Where each repo's docs live

When you need the next level of detail, this is where to go. Cross-repo links
point at each repo's front door, never deep into its internals — internal paths
move, the README is stable.

| You want… | Go to | Repo |
|---|---|---|
| Use the device end to end | [getting-started tutorial](../tutorials/getting-started.md) | TinkerTab |
| Build + flash the firmware | [dev setup](../how-to/dev-setup.md) · [flash the firmware](../how-to/flash-firmware.md) | TinkerTab |
| The firmware internals + why | [TinkerTab architecture](architecture.md) | TinkerTab |
| Hardware pinout + IC list | [hardware reference](../reference/hardware.md) | TinkerTab |
| Drive the device from a workstation | [debug-server reference](../reference/debug-server.md) | TinkerTab |
| The on-device K144 chain | [the TinkerON / K144 chain](the-tinkeron-k144-chain.md) | TinkerTab |
| The cross-stack system map | [TinkerBox `docs/ARCHITECTURE.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/ARCHITECTURE.md) | TinkerBox |
| The WebSocket wire contract | [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md) | TinkerBox |
| Run / deploy the brain | [TinkerBox README](https://github.com/lorcan35/TinkerBox) | TinkerBox |
| The agent sidecar | [TinkerClaw README](https://github.com/lorcan35/TinkerClaw) | TinkerClaw |
| Website-to-API automation | [PingOS README](https://github.com/lorcan35/PingOS) | PingOS |
| Any unfamiliar term | [GLOSSARY](../../GLOSSARY.md) (carried in every repo) | all |

Two facts make this navigable. First, every repo carries the **same** standard
header, peer block, and [GLOSSARY](../../GLOSSARY.md) core, so a reader who
learns one repo's conventions can read any of them — that is the contract in
[`STYLE.md`](../../STYLE.md). Second, the deep wire contract and the cross-stack
map live **once**, in TinkerBox, and the other repos link to them rather than
copying them.

## Why it's built this way

- **Thin client, fat brain.** The [ESP32-P4](../../GLOSSARY.md) has roughly
  512 KB of tight internal SRAM; it cannot host a useful LLM. Pushing all
  intelligence to the Dragon keeps the firmware small, lets the brain upgrade on
  its own cadence, and lets one Dragon serve many faces. The cost is a hard
  dependency on the Dragon for modes 0–3 — which is exactly why the optional
  on-device fallback (mode 4) and the direct-cloud path (mode 5) exist.
- **One WebSocket, many message types.** Multiplexing voice, text, vision, video,
  config, and events over a single persistent connection avoids connection churn
  on a memory-constrained device and keeps the protocol in one place. Binary
  frames carry a 4-byte magic tag (`VID0` video, `AUD0` call audio) or are
  untagged raw PCM bound for STT.
- **Each repo stands alone.** Rather than one umbrella front door, the four repos
  cross-link as peers (the "Part of the Tinker stack" block). Each is fully
  documented on its own, so a contributor can land in any repo and find their
  bearings without first reading three others.
- **Optional pieces are strictly additive.** TinkerON (mode 4), TinkerClaw
  (mode 3), and PingOS are all opt-in. The core product — Tab5 plus Dragon over
  one WebSocket — works with none of them present, and the firmware degrades
  gracefully when an optional module is absent or hot-unplugged.
- **PingOS is family, not a dependency.** It shares the stack's docs conventions
  and peer-links as one of the four repos, but it solves a different problem
  (website automation) and is not required for the voice assistant to work. It is
  on this map so a contributor knows what it is and is *not* — not because a voice
  turn ever touches it.

## See also

- [How the stack fits together](how-the-stack-fits-together.md) · [TinkerTab architecture](architecture.md)
- [Getting started](../tutorials/getting-started.md) · [Dev setup](../how-to/dev-setup.md) · [Flash the firmware](../how-to/flash-firmware.md)
- [Voice-modes reference](../reference/voice-modes.md) · [Debug-server reference](../reference/debug-server.md) · [GLOSSARY](../../GLOSSARY.md)
- System architecture (cross-stack): [TinkerBox `docs/ARCHITECTURE.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/ARCHITECTURE.md)
- Wire protocol: [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md)
- Peer repos: [TinkerBox](https://github.com/lorcan35/TinkerBox) · [TinkerClaw](https://github.com/lorcan35/TinkerClaw) · [PingOS](https://github.com/lorcan35/PingOS)
