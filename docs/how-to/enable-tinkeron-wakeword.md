---
audience: tinkerer
type: how-to
prerequisites: A [Tab5](../../GLOSSARY.md) with the K144/[TinkerON](../../GLOSSARY.md) module stacked on the Module13.2 LLM Mate carrier
last-verified: 2026-05-29
est-time: 10 min
---
# How to enable the TinkerON wakeword

Use this when you want fully hands-free **"Hey Tinker"** wakeword on a
[Tab5](../../GLOSSARY.md). The wakeword runs on the K144/[TinkerON](../../GLOSSARY.md)
module: the Tab5's own microphone array is pumped over the M5-Bus into the K144's
on-device ASR ([`ext_pcm`](../../GLOSSARY.md)), a lenient matcher catches "Hey
Tinker," and a normal voice turn fires.

Assumes the K144 module is **physically stacked correctly** — stack order is
`Tab5 base → Module13.2 LLM Mate carrier → K144`. The Tab5's own USB-C powers
the whole stack through the M5-Bus; the carrier and K144 USB-C ports are
debug-only. Stacking the K144 directly on the Tab5 (no Mate carrier) causes 5V
rail collisions that wedge the AX630C NPU.

## Steps

1. **Boot the Tab5 with the K144 stacked.** The firmware auto-arms the whole
   chain on a clean boot — no manual commands. It auto-detects the K144 baud,
   does a clean `asr.setup` at 115200, then bumps to 1.5 Mbps for the audio
   pump.
2. **Wait for warmup.** Cold-start NPU model load takes ~4.5 s. The chain arms
   the wakeword once the warmup reaches READY.
3. **Say "Hey Tinker."** The K144's sherpa-ncnn streaming ASR renders the phrase
   inconsistently across sessions, so the matcher accepts a family of renderings
   (`tinker` / `thinker` / `hicker` / `hick` / `hanker`). On a match the Tab5
   starts a voice turn via the normal orb-tap path (so it respects your active
   [voice mode](../../GLOSSARY.md) routing).
4. **Speak your request** right after the wake, e.g. _"Hey Tinker — what time is
   it?"_ The turn cycles LISTENING → PROCESSING → SPEAKING → READY.

Self-wake is suppressed during a full voice turn (LISTENING / PROCESSING /
RECONNECTING / SPEAKING) plus a 1 s post-turn grace, so Tinker's own TTS reply
does not re-trigger the wakeword. The `ext_pcm` pump also pauses during a turn so
the Tab5 mic routes to the Dragon while the turn runs.

## Verify it worked

Read the chain + wakeword state over the [debug server](../reference/debug-server.md):

```bash
export TOKEN="abcdef1234567890abcdef1234567890"

# 1. Chain health — failover_state_name must be "ready"
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/m5 | python3 -m json.tool
# → {"failover_state":2,"failover_state_name":"ready","uart_baud":1500000, ...}

# 2. ext_pcm pump + wakeword obs (frames_pumped climbing, wakeword_active true)
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/tinkeron/extpcm \
     | python3 -m json.tool
```

A successful wake fires a voice turn — watch `voice.state` go to LISTENING in
the [`/events` ring](../reference/debug-server.md), and the serial log shows the
ASR rendering it matched.

## Troubleshooting

- **`failover_state_name` is `unavailable`** → The K144 daemon wedged or never
  warmed. Recover with `POST /m5/reset` (sends `sys.reset` to the
  [StackFlow](../../GLOSSARY.md) daemon and re-runs warmup), then re-poll
  `GET /m5`. The watchdog also auto-recovers, escalating to `sys.reboot` after 2
  failed resets.
- **Boot, but wakeword never arms** → Confirm the stack order (Mate carrier
  required) and that warmup reached READY. The chain arms the wakeword on warmup
  READY and on every successful reset.
- **"Hey Tinker" not recognized** → The K144's ASR rendering may be outside the
  matcher family. Check the serial log for the rendered partials; the matcher is
  open-vocabulary (one string, no retraining) so the renderings list can be
  widened.
- **Double-fires / self-wake from the reply** → Suppression covers the full turn
  + 1 s grace; if you still see it, the turn state may have snapped back early.
  Check `voice.state` transitions in `/events`.
- **It worked, then stopped after hours** → K144 daemon state accumulation. The
  firmware does a preventive `sys.reboot` every ~4 h of READY uptime; a manual
  `POST /m5/reset` clears a stuck daemon immediately. UART framing errors at
  1.5 Mbps are real but the JSON parser recovers; a periodic UART resync pause
  prevents accumulation.

## See also

- [Recover a stuck device](recover-a-stuck-device.md) — full K144 recovery
  ladder.
- [The TinkerON / K144 chain](../explanation/the-tinkeron-k144-chain.md) — how
  the on-device chain works and why.
- [Switch voice modes](switch-voice-modes.md) — Onboard (mode 4) runs the whole
  turn on the K144 with no Dragon.
- [Debug server reference](../reference/debug-server.md) — `/m5`, `/m5/reset`.
