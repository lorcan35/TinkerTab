---
audience: tinkerer
type: how-to
prerequisites: [getting-started tutorial](../tutorials/getting-started.md), a [Tab5](../../GLOSSARY.md) booted and connected to Wi-Fi
last-verified: 2026-05-29
est-time: 5 min
---
# Switch voice modes

Use this when you want to change how a [Tab5](../../GLOSSARY.md) runs a voice
turn — trading privacy, speed, and cost. The Tab5 has six
[voice modes](../../GLOSSARY.md): from fully local (free, slow, private) to fully
cloud (fast, paid). This guide shows you what each mode does and three ways to
switch: the on-device mode chip, the mode-picker sheet, and the
[debug server](../reference/debug-server.md)'s `/mode` endpoint.

Assumes the Tab5 is booted, on Wi-Fi, and — for modes that need it — paired with
a [Dragon](../../GLOSSARY.md). Modes 4 and 5 run with no Dragon at all.

## The six tiers

| Mode | Name | STT | LLM | TTS | Latency | Cost | Needs Dragon? |
|---|---|---|---|---|---|---|---|
| **0** | Local | Moonshine | Dragon-local (NPU/Ollama, default ministral-3:3b) | Piper | 60–90 s | Free | Yes |
| **1** | Hybrid | OpenRouter `gpt-audio-mini` | Dragon-local | OpenRouter `gpt-audio-mini` | 4–8 s | ~$0.02/req | Yes |
| **2** | Full Cloud | OpenRouter `gpt-audio-mini` | User-selected (Haiku / Sonnet / GPT-4o) | OpenRouter `gpt-audio-mini` | 3–6 s | $0.03–0.08/req | Yes |
| **3** | TinkerClaw | Moonshine (or OpenRouter) | TinkerClaw Gateway | Piper (or OpenRouter) | varies | varies | Yes |
| **4** | Onboard (K144) | K144 sherpa-ncnn ASR | K144 LLM (qwen2.5-0.5B over UART) | K144 per-utterance synth | 2–3 s | Free | **No** |
| **5** | Solo Direct | OpenRouter | OpenRouter (`or_mdl_llm`) | OpenRouter (`or_mdl_tts`, `or_voice`) | 3–6 s | $0.03–0.08/req | **No** |

What each one is for:

- **Local (0)** — the default. Everything runs on the Dragon, nothing leaves
  your network. Slow on the Q6A's NPU (~60–90 s/turn) but free and fully
  private.
- **Hybrid (1)** — keep the LLM on the Dragon but send speech to OpenRouter for
  fast, accurate STT/TTS. Best dollar-per-turn for a snappy assistant.
- **Full Cloud (2)** — STT, LLM, and TTS all go to OpenRouter. Pick the cloud
  model with the LLM model picker (only enabled in this mode). Fastest and
  highest-quality; you pay per request.
- **TinkerClaw (3)** — the [TinkerClaw](../../GLOSSARY.md) gateway runs the turn
  (its own LLM choice + tools + browser automation); the Dragon becomes an audio
  pipe. Use this for agentic tasks.
- **Onboard / K144 (4)** — the [TinkerON](../../GLOSSARY.md) module runs the
  whole turn on-device over the M5-Bus. No Dragon needed. Requires the K144
  module stacked and warm (~4.5 s cold-start). Tab5-side-only.
- **Solo Direct (5)** — the Tab5 talks straight to OpenRouter for STT/LLM/TTS
  with no Dragon, using the `or_*` NVS keys + on-device RAG against
  `/sdcard/rag.bin`. Needs an OpenRouter API key in NVS. Tab5-side-only.

Modes 4 and 5 are **Tab5-side-only**: when the Tab5 reports its mode over the
wire it downconverts them to `0` so the Dragon never sees them. The
[NVS](../../GLOSSARY.md) key `vmode` still holds the true value (0–5). This is by
design — see the [WebSocket protocol](../reference/debug-server.md) note in the
troubleshooting section.

## Steps

Pick whichever path fits the situation.

### Option A — the mode chip (one-tap cycle)

1. On the home screen, find the **mode chip** below the orb. It shows the
   current mode name (for example, `Local`).
2. **Tap it** to cycle to the next mode, or **long-press the orb** to open the
   full mode-picker sheet (Option B). The chip repaints to the new mode and a
   `mode_switch` audio cue plays.

The chat overlay carries the same affordance: the **mode badge** in the chat top
bar cycles Local / Hybrid / Cloud on tap.

### Option B — the mode-picker sheet

1. **Long-press the orb** on home (or open **Settings → Voice**).
2. The mode sheet lists the tiers. Tap the mode you want.
3. For **Full Cloud (2)**, the **LLM model picker** is enabled — pick
   Claude Haiku / Claude Sonnet / GPT-4o mini. The choice persists to the
   `llm_mdl` NVS key.
4. For **Solo Direct (5)**, if no OpenRouter key is set the UI prompts you to
   scan a QR code (or POST the key — see the
   [NVS settings reference](../reference/nvs-settings.md), `or_key`).

The new mode persists immediately to NVS, so it survives a reboot.

### Option C — the debug server `/mode` endpoint

Switch a Tab5 remotely over HTTP. You need the bearer token (printed to the
serial log on every boot) and the device IP. See the
[debug server reference](../reference/debug-server.md) for token + discovery
details.

```bash
# Export the token once
export TOKEN="abcdef1234567890abcdef1234567890"

# m = the tier number (0-5)
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://<ip>:8080/mode?m=0"   # Local
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://<ip>:8080/mode?m=1"   # Hybrid
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://<ip>:8080/mode?m=3"   # TinkerClaw

# Full Cloud takes an optional model id
curl -s -H "Authorization: Bearer $TOKEN" -X POST \
     "http://<ip>:8080/mode?m=2&model=anthropic/claude-sonnet-4-20250514"

# Onboard (K144) and Solo Direct are Tab5-side-only modes
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://<ip>:8080/mode?m=4"   # Onboard
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://<ip>:8080/mode?m=5"   # Solo Direct
```

## Verify it worked

Read back the live voice state and the persisted `vmode` key:

```bash
# 1. Voice state — confirms the WS is connected and shows the state machine
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/voice | python3 -m json.tool

# 2. The persisted mode — vmode holds the true tier (0-5), even for 4/5
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/settings | python3 -m json.tool | grep vmode
# → "vmode": 4
```

You can also watch the switch fire as an observability event. A mode change
plays the `mode_switch` cue, which records a `ui.cue` event in the
[`/events` ring](../reference/debug-server.md):

```bash
curl -s -H "Authorization: Bearer $TOKEN" "http://<ip>:8080/events?since=0" | python3 -m json.tool
# → look for {"kind":"ui.cue","detail":"mode_switch err=0"}
```

Then send a test turn and confirm a reply comes back through the new pipeline:

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/chat \
     -d '{"text":"reply with PONG"}'
# Poll /voice for last_llm_text, or wait for the chat.llm_done event.
```

## Troubleshooting

- **Mode switched but Dragon shows mode 0 in its logs** → Expected for
  **Onboard (4)** and **Solo Direct (5)**. The Tab5 downconverts those to `0` on
  the wire so the Dragon never sees them; `voice.c` also filters out the Dragon's
  ACK echo to keep local NVS at the true 4/5 value. Check `vmode` in
  `GET /settings`, not the Dragon side. (`vmode` accepts 0–5; the
  [NVS table](../reference/nvs-settings.md) documents the 0–4 range from before
  Solo Direct landed — 5 is valid and live.)
- **Onboard (4) refuses or falls back** → The K144 module must be stacked on the
  Module13.2 LLM Mate carrier and **warm**. Check the chain with
  `GET /m5` — `failover_state_name` must be `ready`. If it is `unavailable`,
  recover with `POST /m5/reset` and re-poll `GET /m5`. See the
  [debug server reference](../reference/debug-server.md) K144 section.
- **Solo Direct (5) does nothing / prompts for a QR scan** → No OpenRouter key
  in NVS. The route returns `SOLO_NO_KEY` until you set `or_key`. Provision it by
  scanning the QR prompt or `POST /settings` (see the `or_*` keys in the
  [NVS settings reference](../reference/nvs-settings.md)).
- **Full Cloud (2) model picker is greyed out** → The LLM model picker is only
  enabled in Full Cloud. Switch to mode 2 first, then choose the model.
- **Cloud STT/TTS suddenly reverts to Local mid-session** → Auto-fallback. If a
  cloud STT/TTS request fails, the Dragon falls back to local for that request
  and sends a `config_update` with an `error` field; the Tab5 then auto-reverts
  to Local. Re-select the cloud mode once connectivity is back.
- **Switch refused mid-turn** → Changing the mode while a voice turn is in flight
  auto-cancels the turn with a toast (the nav/voice chokepoint calls
  `voice_cancel`). Wait for the orb to return to its idle state, then switch.
- **Hit the daily spend cap** → The `cap_mils` NVS key caps daily LLM spend
  (default $1.00/day). Exceeding it triggers a `cap_downgrade`, pulling you back
  off paid cloud tiers. Raise the cap via `POST /settings` if you want to spend
  more.

## See also

- [Voice mode definitions](../../GLOSSARY.md) — the canonical one-line spec for
  each tier.
- [Debug server reference](../reference/debug-server.md) — full `/mode`,
  `/voice`, `/m5`, and `/events` endpoint detail.
- [NVS settings reference](../reference/nvs-settings.md) — `vmode`, `llm_mdl`,
  `or_*`, and `cap_mils` keys.
- [How the stack fits together](../explanation/how-the-stack-fits-together.md) —
  why the Tab5 is a thin client and the Dragon is the brain.
