---
audience: developer
type: explanation
prerequisites: none
last-verified: 2026-05-29
---
# The TinkerON / K144 chain — how on-device voice works and why

## The question

The [Tab5](../../GLOSSARY.md) can run a whole voice turn — wakeword, ASR, LLM,
TTS — with **no [Dragon](../../GLOSSARY.md) at all**, on a small module stacked
on its back. This page explains how that on-device chain works, why it talks the
[StackFlow](../../GLOSSARY.md) protocol over a UART, how it self-heals when the
module wedges, and why it is strictly optional. To *enable* it, see
[Enable the TinkerON wakeword](../how-to/enable-tinkeron-wakeword.md).

## The model

### What the module is

**K144 / [TinkerON](../../GLOSSARY.md)** is an M5Stack LLM Module Kit built
around an **AX630C NPU** (4 GB, 3.2 TOPS). "TinkerON" is the user-facing brand;
**K144 / AX630C / sherpa-ncnn** are the canonical hardware identifiers in logs
and code. It runs its own Linux + a service daemon and ships with on-device
models: a streaming ASR (`sherpa-ncnn` zipformer), a KWS model, a small LLM
(`qwen2.5-0.5B`), a TTS voice, and YOLO detection.

### The physical stack

```
   ┌──────────────┐
   │   K144        │  AX630C NPU — ASR + LLM + TTS + KWS + YOLO
   └──────┬───────┘
          │  M5-Bus (UART Port C, or USB transport)
   ┌──────┴───────┐
   │ Module13.2    │  LLM Mate carrier  ── REQUIRED
   │ LLM Mate      │  (passes M5-Bus through; provides power feed)
   └──────┬───────┘
          │  M5-Bus
   ┌──────┴───────┐
   │   Tab5        │  its own USB-C powers the whole stack (~1.5 W K144 load)
   └──────────────┘
```

The Mate carrier is not optional: stacking the K144 directly on the Tab5
collides the 5V rails and wedges the NPU silicon. The Tab5's own USB-C powers
everything through the M5-Bus.

### The transport and the protocol

The Tab5 talks to the K144 over the M5-Bus — historically a **UART** on Port C
(TX GPIO 6, RX GPIO 7), starting at 115200 8N1 and bumping to **1.5 Mbps** for
audio throughput; a later pivot added a **USB functionfs** transport (two
userspace bridges on the K144 for StackFlow JSON + YOLO frames). Either way the
wire protocol is **StackFlow**: newline-delimited JSON, one socket surviving
many inference calls (no length-prefix framing).

```
setup  ──►  ack: data:"None", then a work_id (e.g. "llm.1000")
infer  ──►  stream: {"object":"llm.utf-8.stream","data":{"delta":...,"finish":bool}}
```

Verified `sys.*` verbs: `sys.ping`, `sys.hwinfo`, `sys.lsmode`, `sys.reset`,
`sys.reboot`, `sys.version`. (`sys.list` / `sys.status` / `sys.uptime` /
`sys.log` are **not** real — probed.)

### The two on-device features

| Feature | What it does |
|---|---|
| **Always-on wakeword** | The Tab5's own mic array is pumped over the M5-Bus into the K144's ASR ([`ext_pcm`](../../GLOSSARY.md)). A lenient matcher catches "Hey Tinker" and fires a normal voice turn. |
| **Onboard turn (voice mode 4)** | ASR + LLM + TTS all run on the K144. A user speaks at the stack → on-device ASR + LLM → per-utterance TTS streamed back over the transport → the Tab5 upsamples and plays it. No Dragon. |

The wakeword fires through the regular orb-tap path, so the actual turn still
respects the active [voice mode](../../GLOSSARY.md) routing. The matcher is
**open-vocabulary**: it accepts a family of ASR renderings
(`tinker` / `thinker` / `hicker` / `hick` / `hanker`) because the streaming
zipformer renders "Hey Tinker" inconsistently — changing the wake phrase is a
one-string edit, no model retraining.

### Self-healing

The K144 daemon wedges intermittently. The chain manages this proactively rather
than relying on a human:

- **Warmup gate.** Boot runs a probe + one synchronous inference to map the model
  into NPU memory (up to a 6-minute cold-start budget). Success flips the
  failover gate READY; any failure flips it UNAVAILABLE.
- **Reactive watchdog.** Polls the chain; on a stall it escalates
  `kick → unavailable_kick → sys.reset → sys.reboot` (full K144 Linux reboot
  after repeated failures). All automatic, rate-limited.
- **Preventive maintenance.** A `sys.reboot` every ~4 h of READY uptime (in a
  quiet window) clears accumulated daemon state; a periodic UART resync pause
  gives the receiver an idle window to re-lock the bit clock at 1.5 Mbps.
- **Suppression.** Wakeword is suppressed during a full voice turn + 1 s grace so
  Tinker's own TTS reply does not self-wake; the `ext_pcm` pump pauses during a
  turn so the Tab5 mic routes to the Dragon.

The whole surface is observable via `GET /m5`, `POST /m5/reset`, and the
`m5.*` / `error.k144` [observability events](../reference/observability-events.md).

### Modularity rules (non-negotiable)

The K144 is an addon. Six rules keep the Tab5 from ever depending on it:
boot-path-agnostic, no feature regresses when it is absent, capability detection
gates everything, the UI grays out absent features, hot-unplug is graceful, and
no `#ifdef` guards. The Tab5 is a complete product without a K144.

## Why it's built this way

- **A real offline fallback.** Modes 0–3 hard-depend on the Dragon. The K144
  gives the Tab5 a genuine no-Dragon path (mode 4) plus hands-free wakeword — the
  product's headline feature — without forcing every user to buy the module.

- **StackFlow over a custom protocol.** The K144 ships its daemon and JSON wire
  format; meeting it where it is (newline-delimited JSON, `work_id` handles) was
  cheaper and more robust than rebuilding the K144 firmware. The JSON parser
  tolerates the UART framing errors that 1.5 Mbps produces.

- **Manage the hardware quirks, don't pretend they're gone.** The 1.5 Mbps clock
  drift, daemon state accumulation, and ES7210 codec drift are real. The chain's
  watchdog + preventive reboots + resync pause *manage* them so the device stays
  always-on; they are not claimed to be fixed.

- **Lenient matching beats a brittle model.** A retrained custom wake model would
  be more precise but locks the wake phrase in silicon. An open-vocabulary string
  matcher over the ASR output trades a little precision for the freedom to change
  the phrase in one line.

## See also

- [Enable the TinkerON wakeword](../how-to/enable-tinkeron-wakeword.md) ·
  [Recover a stuck device](../how-to/recover-a-stuck-device.md)
- [Voice modes reference](../reference/voice-modes.md) (mode 4) ·
  [Debug server reference](../reference/debug-server.md) (`/m5`)
- [How the stack fits together](how-the-stack-fits-together.md) ·
  [The voice pipeline](the-voice-pipeline.md)
