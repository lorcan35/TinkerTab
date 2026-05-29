---
audience: tinkerer
type: how-to
prerequisites: [getting-started tutorial](../tutorials/getting-started.md), a [Tab5](../../GLOSSARY.md) booted and on Wi-Fi
last-verified: 2026-05-29
est-time: 5 min
---
# Choose your voice mode

Use this when you have decided you need to change voice modes but cannot decide
**which** of the six tiers to pick. The [Tab5](../../GLOSSARY.md) runs a voice
turn through one of six [voice modes](../../GLOSSARY.md), and they trade off
along four axes that pull against each other: **privacy**, **latency**, **cost**,
and whether it works **offline** (with no [Dragon](../../GLOSSARY.md) on the
network). This guide helps you pick; the separate
[Switch voice modes](switch-voice-modes.md) guide shows you the buttons to press
once you have.

If you just want the authoritative spec for each tier — STT/LLM/TTS per stage,
on-the-wire behavior, routing codes — read the
[voice modes reference](../reference/voice-modes.md) instead. This page is a
decision aid, not a spec.

## The four axes that matter

Every mode is a point on these four axes. Decide which one you care about most
right now, then read down the matching column.

- **Privacy** — does any audio or text leave your network? Local (0) and Onboard
  (4) keep everything on hardware you own. Hybrid (1) sends speech to the cloud
  but keeps the LLM on your Dragon. Full Cloud (2), TinkerClaw with cloud models
  (3), and Solo Direct (5) send your turn to a third party (OpenRouter).
- **Latency** — how long from "stop talking" to "hear the answer." On-device and
  cloud are fast (2–8 s); the fully-local Dragon path is slow (60–90 s) because
  the Q6A NPU runs the LLM itself.
- **Cost** — Local, Onboard, and the Dragon-local half of Hybrid are free. The
  cloud tiers bill per request through OpenRouter; the daily spend is capped by
  the `cap_mils` [NVS](../../GLOSSARY.md) key (default $1.00/day).
- **Offline** — does the mode need a Dragon reachable on the network at all?
  Onboard (4) and Solo Direct (5) are the only two that run a complete turn with
  **no Dragon**: Onboard on the K144 module, Solo Direct straight to OpenRouter.

## The trade-off matrix

| `vmode` | Mode | Privacy | Latency | Cost | Offline (no Dragon)? |
|---|---|---|---|---|---|
| 0 | Local | Full — nothing leaves your network | 60–90 s | Free | No |
| 1 | Hybrid | LLM stays local; speech goes to cloud | 4–8 s | ~$0.02/req | No |
| 2 | Full Cloud | None — STT + LLM + TTS all cloud | 3–6 s | Free | No (needs Dragon as audio pipe) |
| 3 | TinkerClaw | Depends on the gateway's model choice | varies | varies | No (needs Dragon as audio pipe) |
| 4 | Onboard (K144) | Full — runs on the stacked module | 2–3 s | Free | **Yes** |
| 5 | Solo Direct | None — Tab5 → OpenRouter directly | 3–6 s | $0.03–0.08/req | **Yes** |

Two modes are **Tab5-side-only**: Onboard (4) and Solo Direct (5). When the Tab5
reports its mode over the wire it downconverts those to `0` so the Dragon never
sees them — the `vmode` NVS key still holds the true value. That is why these two
can run with no Dragon at all.

## Pick this if…

| If you want… | Pick | Why |
|---|---|---|
| Maximum privacy, and you don't mind waiting | **Local (0)** | The default. STT (Moonshine), LLM, and TTS (Piper) all run on your Dragon. Nothing leaves your network. Slow on the Q6A NPU but free and fully private. |
| A snappy assistant for the lowest cost | **Hybrid (1)** | Keeps the LLM on your Dragon for privacy + free inference, but sends speech to OpenRouter for fast, accurate STT/TTS. Best dollar-per-turn. |
| The fastest, highest-quality answers | **Full Cloud (2)** | STT, LLM, and TTS all go to OpenRouter. The LLM model picker (Haiku / Sonnet / GPT-4o) is **only enabled in this mode**. You pay per request. |
| Tools, browser automation, agentic tasks | **TinkerClaw (3)** | The [TinkerClaw](../../GLOSSARY.md) gateway runs the turn with its own LLM choice plus tools and browser automation; the Dragon becomes an audio pipe. |
| Hands-free, on-device, no Dragon at all | **Onboard / K144 (4)** | The [TinkerON](../../GLOSSARY.md) module runs the whole turn over the M5-Bus. Free, private, ~2–3 s, works with the Dragon off. Requires the K144 stacked and warm (~4.5 s cold-start). |
| The Tab5 to work away from your Dragon, with cloud quality | **Solo Direct (5)** | The Tab5 talks straight to OpenRouter for STT/LLM/TTS using the `or_*` NVS keys, plus on-device RAG against `/sdcard/rag.bin`. Needs an OpenRouter API key. |

### Quick decision shortcuts

- **"I value privacy above everything."** → Local (0) when the Dragon is on, or
  Onboard (4) for fast + private + offline.
- **"I want it fast and cheap, and I trust my Dragon."** → Hybrid (1).
- **"I want the smartest possible answer."** → Full Cloud (2) with Sonnet.
- **"I'm away from home / the Dragon is off."** → Onboard (4) if the K144 is
  stacked; otherwise Solo Direct (5).
- **"I need it to actually *do* things (browse, call tools)."** → TinkerClaw (3).

## How to switch once you've chosen

You do not change modes here — the mechanics live in their own guide. There are
three ways:

1. **The mode chip** below the orb on home — tap to cycle.
2. **The mode-picker sheet** — long-press the orb (or Settings → Voice).
3. **The [debug server](../reference/debug-server.md) `/mode` endpoint** — switch
   a device remotely over HTTP.

The full step-by-step, including the Full Cloud model picker and the OpenRouter
key prompt for Solo Direct, is in
[Switch voice modes](switch-voice-modes.md).

## Verify it worked

After switching, confirm the persisted tier matches what you chose:

```bash
export TOKEN="abcdef1234567890abcdef1234567890"
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/settings \
     | python3 -m json.tool | grep vmode
# → "vmode": 4
```

The `vmode` key holds the true tier (0–5), even for Onboard (4) and Solo Direct
(5) which report as `0` over the wire to the Dragon.

## Troubleshooting

- **Picked Onboard (4) but it won't run** → The K144/[TinkerON](../../GLOSSARY.md)
  module must be stacked on the Module13.2 LLM Mate carrier and **warm**. Check
  `GET /m5` — `failover_state_name` must be `ready`. Recover with
  `POST /m5/reset`. See [Enable the TinkerON wakeword](enable-tinkeron-wakeword.md)
  and [Recover a stuck device](recover-a-stuck-device.md).
- **Picked Solo Direct (5) but it prompts for a QR scan** → No OpenRouter key in
  NVS. The route returns `SOLO_NO_KEY` until you set `or_key`. See the `or_*`
  keys in the [NVS settings reference](../reference/nvs-settings.md).
- **Picked a cloud tier but it reverts to Local mid-session** → Auto-fallback. If
  a cloud STT/TTS request fails, the Dragon falls back to local for that request
  and sends a `config_update` with an `error` field, and the Tab5 reverts to
  Local. Re-select the cloud mode once connectivity is back.
- **Hit the daily spend cap on a cloud tier** → The `cap_mils` NVS key caps daily
  LLM spend (default $1.00/day). Exceeding it triggers a `cap_downgrade` off the
  paid tiers. Raise the cap via `POST /settings` if you want to spend more.

## See also

- [Switch voice modes](switch-voice-modes.md) — the mechanics: chip, sheet, and
  the `/mode` endpoint.
- [Voice modes reference](../reference/voice-modes.md) — the authoritative spec:
  STT/LLM/TTS per stage, on-the-wire behavior, routing codes, failover guards.
- [Enable the TinkerON wakeword](enable-tinkeron-wakeword.md) — go fully
  hands-free with Onboard (4) on the K144 module.
- [NVS settings reference](../reference/nvs-settings.md) — `vmode`, `llm_mdl`,
  `or_*`, and `cap_mils`.
- [How the stack fits together](../explanation/how-the-stack-fits-together.md) —
  why the Tab5 is a thin client and the Dragon is the brain.
- The Dragon's side of these modes: see
  [**TinkerBox**](https://github.com/lorcan35/TinkerBox)'s
  [`docs/reference/voice-modes.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/reference/voice-modes.md)
  for what each tier costs the Dragon's pipeline.
