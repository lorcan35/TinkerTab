---
audience: integrator
type: reference
prerequisites: none
last-verified: 2026-05-29
---
# Voice modes reference

Authoritative, lookup-oriented reference for the [Tab5](../../GLOSSARY.md)'s
six [voice modes](../../GLOSSARY.md). No tutorials here — to change a mode see
[Switch voice modes](../how-to/switch-voice-modes.md).

A voice mode decides **where** the three pipeline stages — speech-to-text (STT),
the LLM, and text-to-speech (TTS) — run for a given turn. The active mode is the
[NVS](../../GLOSSARY.md) key `vmode` (0–5). Routing lives in
[`main/voice_modes.c`](../../main/voice_modes.c).

## The six tiers

| `vmode` | Mode | STT | LLM | TTS | Latency | Cost | Needs Dragon? |
|---|---|---|---|---|---|---|---|
| 0 | Local | Moonshine (Dragon) | Dragon-local (NPU / llama-server / Ollama, default ministral-3:3b ~67 s/turn median) | Piper (Dragon) | 60–90 s | Free | Yes |
| 1 | Hybrid | OpenRouter `gpt-audio-mini` | Dragon-local | OpenRouter `gpt-audio-mini` | 4–8 s | ~$0.02/req | Yes |
| 2 | Full Cloud | OpenRouter `gpt-audio-mini` | User-selected (`llm_mdl`: Haiku / Sonnet / GPT-4o) | OpenRouter `gpt-audio-mini` | 3–6 s | $0.03–0.08/req | Yes (audio pipe) |
| 3 | TinkerClaw | Moonshine (or OpenRouter) | [TinkerClaw](../../GLOSSARY.md) gateway | Piper (or OpenRouter) | varies | varies | Yes (audio pipe) |
| 4 | Onboard (K144) | K144 sherpa-ncnn ASR (chain) or text-only (failover) | K144 stacked LLM (qwen2.5-0.5B over UART) | K144 `single_speaker_english_fast` | 2–3 s | Free | **No** |
| 5 | Solo Direct | OpenRouter (`or_mdl_stt`) | OpenRouter (`or_mdl_llm`) | OpenRouter (`or_mdl_tts`, `or_voice`) | 3–6 s | $0.03–0.08/req | **No** |

## On-the-wire behavior

| Aspect | Detail |
|---|---|
| Wire value range | `config_update.voice_mode` is always `0..3`. |
| Modes 4 + 5 | Tab5-side-only. They downconvert to `0` on the wire so the Dragon never sees them. |
| ACK echo filtering | `voice.c` filters the Dragon's `config_update` ACK echo so local NVS `vmode` keeps the true 4/5 value. |
| Persistence | `vmode` (NVS); `llm_mdl` for the Cloud model; `or_*` keys for Solo Direct. |

## Per-mode notes

| Mode | Notes |
|---|---|
| Local (0) | The default. Nothing leaves your network. Slow on the Q6A NPU but free + fully private. |
| Hybrid (1) | LLM stays Dragon-local; speech goes to OpenRouter for fast, accurate STT/TTS. Best dollar-per-turn for snappy. |
| Full Cloud (2) | The LLM model picker is **only enabled in this mode** (writes `llm_mdl`). |
| TinkerClaw (3) | The TinkerClaw gateway (`localhost:18789`) runs the turn with its own LLM + tools + browser automation; the Dragon becomes an audio pipe. |
| Onboard (4) | Requires the K144 module stacked + warm (~4.5 s cold-start). Used for failover (Local → Onboard when Dragon is unreachable ≥30 s + K144 warm) and as a chosen mode (every turn → K144). |
| Solo Direct (5) | Tab5 → OpenRouter directly, no Dragon. Needs `or_key`. On-device RAG against `/sdcard/rag.bin`. TT #379 multimodal audio chat (`or_mdl_audio`) collapses STT→LLM→TTS into one `/chat/completions` call. |

## Routing decision codes

`voice_modes_route_text` returns a route per turn:

| Route | When |
|---|---|
| `LOCAL` / `HYBRID` / `CLOUD` / `TINKERCLAW` | Normal dispatch for modes 0–3. |
| `LOCAL_ONBOARD` | Local-mode failover to the K144 (Dragon unreachable ≥30 s + K144 warm). |
| `SOLO_DIRECT` | Mode 5 with a valid `or_key`. |
| `SOLO_NO_KEY` | Mode 5 with no OpenRouter key set — the UI prompts a QR scan. |

## Failover + guards

| Behavior | Detail |
|---|---|
| Cloud STT/TTS auto-fallback | If a cloud STT/TTS request fails, the Dragon falls back to local for that request and sends `config_update` with an `error` field → the Tab5 auto-reverts to Local. |
| Local → Onboard failover | `vmode=0` + Dragon WS unreachable ≥30 s + K144 warm + a text turn → routed to the K144. Toast: "Using onboard LLM"; reverts on Dragon reconnect. |
| Mode change mid-turn | Auto-cancels the in-flight turn with a toast (the nav/voice chokepoint calls `voice_cancel`). |
| Daily spend cap | `cap_mils` (default $1.00/day) — exceeding it triggers `cap_downgrade`, pulling off paid tiers. |

## Examples

Switch over the [debug server](debug-server.md):

```bash
export TOKEN="abcdef1234567890abcdef1234567890"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://<ip>:8080/mode?m=1"   # Hybrid
curl -s -H "Authorization: Bearer $TOKEN" -X POST \
     "http://<ip>:8080/mode?m=2&model=anthropic/claude-sonnet-4-20250514"        # Cloud + model
```

Confirm the persisted tier:

```bash
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/settings \
     | python3 -m json.tool | grep vmode
# → "vmode": 4
```

## See also

- [Switch voice modes](../how-to/switch-voice-modes.md) — the task guide.
- [NVS settings reference](nvs-settings.md) — `vmode`, `llm_mdl`, `or_*`, `cap_mils`.
- [WebSocket protocol](websocket-protocol.md) — `config_update` + mode downconversion.
- [The voice pipeline](../explanation/the-voice-pipeline.md) — what each stage does.
