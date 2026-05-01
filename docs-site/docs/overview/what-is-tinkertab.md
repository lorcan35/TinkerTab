---
title: What is TinkerTab?
sidebar_label: What is TinkerTab?
---

# What is TinkerTab?

A privacy-conscious voice assistant that runs on hardware you own. The product is voice-first: tap a glowing orb on the screen, ask a question, get a spoken answer. Cloud AI providers are *optional* — the default mode runs everything locally and never touches the internet.

## The split-brain architecture

There are two pieces, on purpose:

```
┌─────────────────────────┐         ┌──────────────────────────────┐
│  Tab5 — the face         │         │  Dragon Q6A — the brain      │
│                          │         │                              │
│  • 5" portrait touch     │  ◄───► │  • STT (Moonshine / cloud)   │
│  • Mic array + speaker   │   WS    │  • LLM (Ollama / cloud)      │
│  • Camera                │  3502   │  • TTS (Piper / cloud)       │
│  • LVGL UI in C/ESP-IDF  │         │  • Memory + RAG              │
│  • No AI logic           │         │  • Skills + scheduler        │
└─────────────────────────┘         └──────────────────────────────┘
```

**Tab5** runs the [TinkerOS firmware](https://github.com/lorcan35/TinkerTab) on an ESP32-P4. It owns the display, the microphones, the speaker, the camera, the touch panel. It does *no* AI inference itself — it streams audio, text, and images to Dragon over a single WebSocket and renders the responses.

**Dragon Q6A** is a small Linux box on your home LAN running the [TinkerBox server](https://github.com/lorcan35/TinkerBox). It owns the speech-to-text, the LLM, the text-to-speech, the conversation engine, the skill scheduler, the dashboard. By default it runs *everything* locally — Moonshine STT, ministral-3:3b on Ollama, Piper TTS — and never reaches out to a cloud API.

## Five voice modes

Pick your privacy/speed tradeoff per session, or even per turn:

| Mode | STT | LLM | TTS | Cost | Latency |
|------|-----|-----|-----|------|---------|
| **Local** | Moonshine | Ollama / NPU | Piper | $0 | 60–90 s |
| **Hybrid** | OpenRouter | Local | OpenRouter | ~$0.02/turn | 4–8 s |
| **Full Cloud** | OpenRouter | OpenRouter | OpenRouter | $0.03–$0.08/turn | 3–6 s |
| **TinkerClaw** | Moonshine | Gateway agent | Piper | varies | varies |
| **Onboard (K144)** | sherpa-ncnn | qwen2.5-0.5B on K144 NPU | K144 TTS | $0 | 2–3 s |

[See voice modes →](/docs/tasks/voice-modes)

## Why two pieces?

Because the LLM lives on the Dragon, you can:

- **Swap models without reflashing the firmware.** Switch from ministral to claude-haiku to gemini in Settings.
- **Hold conversation context across reboots.** Dragon's MessageStore persists everything in SQLite.
- **Run multimodal.** Send a photo from Tab5; Dragon routes the vision turn to a vision-capable model; ask follow-ups against the same image.
- **Add new skills as Python files.** The Dragon's skill registry hot-loads tools — no firmware involved.

## Who's it for?

Built for tinkerers and hackers as much as for people who just want a voice assistant that doesn't snitch on them. If you're comfortable with USB flashing, SSH, and systemd, you can deploy this in an afternoon and start writing Python skills the same evening.
