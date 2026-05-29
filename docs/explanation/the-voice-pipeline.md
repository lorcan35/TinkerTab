---
audience: developer
type: explanation
prerequisites: none
last-verified: 2026-05-29
---
# The voice pipeline — how a turn becomes an answer

## The question

You tap the orb (or say "Hey Tinker"), speak, and a few seconds — or a minute —
later the [Tab5](../../GLOSSARY.md) answers out loud. This page explains what
happens in between: the three stages a turn passes through, who runs each stage,
and why the [Tab5](../../GLOSSARY.md) is built to be a thin pipe rather than the
thing that thinks. It does not teach you how to *use* it — for that see
[Your first voice conversation](../tutorials/your-first-voice-conversation.md).

## The model

A voice turn is **three stages**: speech-to-text (STT) → the LLM → text-to-speech
(TTS). The Tab5 owns the microphone and speaker at the ends; the work in the
middle happens wherever the active [voice mode](../../GLOSSARY.md) routes it
(usually the [Dragon](../../GLOSSARY.md)).

```
   you speak
      │
      ▼
 ┌─────────────────────────────────────────────────────────────┐
 │  Tab5 (the face)                                            │
 │   ES7210 4-mic ─► I2S TDM RX ─► 48 kHz PCM ─► 3:1 down ─►   │
 │   16 kHz mono PCM frames ──────────────────────────────────┐│
 └────────────────────────────────────────────────────────────┼┘
                                                               │ untagged
                              one WebSocket  ws://host:3502     │ binary frames
                                                               ▼
 ┌─────────────────────────────────────────────────────────────┐
 │  Dragon (the brain)                                         │
 │   STT (Moonshine) ─► transcript                             │
 │        │                                                    │
 │        ▼                                                    │
 │   LLM (llama-server / cloud) ─► response text               │
 │        │                                                    │
 │        ▼                                                    │
 │   TTS (Piper / Kokoro) ─► 16 kHz PCM ──────────────────────┐│
 └────────────────────────────────────────────────────────────┼┘
                                                               │ tts_start
                                                               │ binary PCM
                                                               │ tts_end
 ┌─────────────────────────────────────────────────────────────┼┘
 │  Tab5                                                       ▼
 │   1:3 upsample ─► 48 kHz ─► I2S STD TX ─► ES8388 ─► speaker  │
 └─────────────────────────────────────────────────────────────┘
      │
      ▼
   you hear the answer
```

### The state machine

The Tab5 tracks a turn through a state machine in `voice.c`, surfaced on every
transition as a `voice.state` [observability event](../reference/observability-events.md):

```
IDLE ─► CONNECTING ─► READY ─► LISTENING ─► PROCESSING ─► SPEAKING ─► READY
                                   │                          │
                                   └──── cancel ──────────────┘──► READY/IDLE
                        (RECONNECTING if the WS drops mid-turn)
```

- **LISTENING** — mic is streaming untagged 16 kHz PCM frames up the
  [WebSocket](../../GLOSSARY.md).
- **PROCESSING** — the Dragon is running STT + the LLM; the Tab5 sends keepalive
  pings every ~15 s.
- **SPEAKING** — TTS audio is streaming down (`tts_start` → binary PCM →
  `tts_end`) and draining through the playback task.

### Audio, end to end

The hardware runs at **48 kHz**; STT wants **16 kHz**. The mic path downsamples
3:1 on the way up and the playback path upsamples 1:3 on the way down. Capture
is a 4-slot TDM read off the ES7210 (slot 0 = primary mic); playback is an STD
Philips write to the ES8388. A dedicated playback drain task does
producer-consumer I2S writes so the WS receive path never blocks on the speaker.

### Three turn shapes

The same pipeline serves three input shapes:

| Shape | How it starts | What is sent | LLM reply? |
|---|---|---|---|
| **Voice (Ask)** | tap / wakeword | `start` → untagged PCM → `stop` | Yes (STT → LLM → TTS) |
| **Dictation** | long-press | `start {"mode":"dictate"}` → PCM | No — STT only, + a `dictation_summary` |
| **Text** | typed | `{"type":"text","content":"..."}` | Yes — skips STT, same context |

### Where the work runs — the six modes

The state machine is identical across modes; only the *backend* of each stage
moves. See the [voice modes reference](../reference/voice-modes.md) for the full
table. The headline: modes 0–3 need the Dragon; mode 4 (Onboard / K144) and
mode 5 (Solo Direct) run a whole turn with **no Dragon**.

## Why it's built this way

- **Thin client, fat brain.** The ESP32-P4's ~512 KB internal SRAM cannot host a
  useful LLM. Keeping STT/LLM/TTS on the Dragon means the model upgrades without
  a reflash and one Dragon can serve a fleet of faces. The cost — a hard Dragon
  dependency for modes 0–3 — is exactly why the on-device fallback (mode 4) and
  direct-cloud path (mode 5) exist.

- **Untagged PCM is the legacy mic contract.** Binary frames without a magic
  prefix are always raw 16 kHz mono PCM going straight to STT. Tagged frames
  (`VID0` video, `AUD0` call audio) were layered on later without breaking that
  contract — see the [WebSocket protocol](../reference/websocket-protocol.md).

- **Dictation is STT-only on purpose.** A dictation is content capture, not a
  question. Suppressing the LLM reply and instead post-processing the transcript
  into a title + summary matches what a note-taker wants.

- **Boot-time silent connect.** The WS connects and the device registers at boot
  so the first mic tap does not pay a 5–15 s connect cost. A reconnect watchdog
  with exponential backoff keeps the link alive across Dragon restarts and Wi-Fi
  blips.

- **The encoder is gated, the decoder is ready.** OPUS would shrink the audio
  frames, but the SILK NSQ encoder crashes mid-frame on the ESP32-P4 (issue
  #264), so it is gated off in `voice_codec.h` while the decoder waits for a
  Dragon→Tab5 OPUS TTS path. Wake word + AEC scaffolding (ESP-SR) was removed in
  PR #162; the wakeword we ship today runs on the K144, not on-P4.

## See also

- [Voice modes reference](../reference/voice-modes.md) ·
  [WebSocket protocol](../reference/websocket-protocol.md)
- [Your first voice conversation](../tutorials/your-first-voice-conversation.md) ·
  [Dictate and take notes](../how-to/dictate-and-take-notes.md)
- [TinkerTab architecture](architecture.md) · [The TinkerON / K144 chain](the-tinkeron-k144-chain.md)
- Legacy detail: [`../VOICE_PIPELINE.md`](../VOICE_PIPELINE.md)
