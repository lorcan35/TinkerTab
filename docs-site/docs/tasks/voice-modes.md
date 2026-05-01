---
title: Voice modes
sidebar_label: Voice modes
---

# Voice modes

Five tiers, each picking a different (STT, LLM, TTS) tuple. Switch tiers in Settings → Voice mode, or via the **mode pill** at the top of the home screen (tap to cycle).

![Voice mode picker sheet](/img/mode-sheet.jpg)

## The five tiers

| Mode | Code | STT | LLM | TTS | Cost / turn | Latency |
|------|------|-----|-----|-----|-------------|---------|
| Local | 0 | Moonshine | Ollama (default `ministral-3:3b`) | Piper | $0 | 60–90 s |
| Hybrid | 1 | OpenRouter `gpt-audio-mini` | Local (unchanged) | OpenRouter `gpt-audio-mini` | ~$0.02 | 4–8 s |
| Full Cloud | 2 | OpenRouter | OpenRouter (user-selected) | OpenRouter | $0.03–$0.08 | 3–6 s |
| TinkerClaw | 3 | Moonshine | TinkerClaw Gateway agent | Piper | varies | varies |
| Onboard (K144) | 4 | sherpa-ncnn (K144) | qwen2.5-0.5B (K144 NPU) | K144 single_speaker_english_fast | $0 | 2–3 s |

Costs are approximate, based on current OpenRouter pricing. Latency is end-to-end on a typical home LAN.

## Why pick one over another

**Local (0)** — privacy first. Nothing leaves your network. Slow because the LLM is small and runs on Dragon's CPU. Best for ambient queries where you don't mind a 60–90 s wait.

**Hybrid (1)** — STT and TTS go to OpenRouter (faster + better quality), but the LLM stays local. Good middle ground: 4–8 s latency, ~2 ¢/turn, conversation history still on your Dragon.

**Full Cloud (2)** — everything cloud. Fastest. Use the Settings model picker to choose between Haiku, Sonnet, GPT-4o, or any of the 35+ OpenRouter models the router knows about. No local model loaded; Dragon is just a relay.

**TinkerClaw (3)** — the agentic mode. The LLM call goes to the TinkerClaw Gateway running on the same Dragon, which can plan multi-step tasks, run shell commands, browse the web, and call other skills. STT + TTS still local. Use this when you want the assistant to *do* something, not just *answer*.

**Onboard (4)** — fully offline. Every turn goes through the K144. No Dragon needed at all. Limited because the qwen2.5-0.5B model is tiny — short turns, no tools, no memory. But survives a Dragon outage gracefully.

## Per-modality routing (the Dragon multi-model router)

In voice modes 0/1/2, Dragon's *router* picks the actual LLM per-turn based on what the message needs:

- **Plain text** → cheapest text model in the active tier (e.g. `ministral-3:3b` in Local, `deepseek-v4-flash` in Cloud)
- **Vision (photo)** → vision-capable model (e.g. MiniCPM-V in Local, `qwen3.6-flash` in Cloud)
- **Video** → video-capable model (Gemini 3 Flash in Cloud only — local doesn't have a competitive video model yet)

The active fleet is declared in Dragon's `config.yaml`. See the [router cookbook](https://github.com/lorcan35/TinkerBox/blob/main/docs/router-cookbook.md) for full recipes.

The `fleet_summary` is announced in `session_start.config` so Tab5 can light up vision/video chips dynamically. Older Tab5 firmware ignores it; new firmware uses it.

## Failover behavior

- **Cloud STT/TTS error** → Dragon falls back to Moonshine + Piper for that turn AND sends `config_update.error` → Tab5 auto-reverts to Local mode. Toast: *"Cloud failed, switched to Local"*.
- **Dragon WS disconnect** AND in Local mode AND K144 warm → Tab5 routes the next text turn through K144 transparently. Toast: *"Using onboard LLM"*. Reconnect → next turn returns to Dragon.

## Spend caps

NVS keys `cap_mils` (per-day cap in mils — 1/1000 of a cent), `spent_mils` (today's cumulative). Default cap is 100,000 mils = $1.00/day. Exceeding it triggers a `cap_downgrade` from Cloud → Hybrid → Local.

You can set the cap in Settings → Voice → Spend cap.
