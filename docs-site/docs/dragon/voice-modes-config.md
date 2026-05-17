---
title: Voice-mode config
sidebar_label: Voice-mode config
---

# Voice-mode config

Each voice mode picks a `(STT, LLM, TTS)` tuple. Defaults are sane; override per-mode in `config.yaml`.

## Default tuples

```yaml
voice_modes:
  0:  # Local
    stt: "moonshine"
    llm: "ollama"        # or "router" with fleet[tier=local]
    tts: "piper"
    system_prompt_tokens: 128
    timeout_s: 300

  1:  # Hybrid
    stt: "openrouter"
    llm: "ollama"        # unchanged from local
    tts: "openrouter"
    system_prompt_tokens: 256
    timeout_s: 60

  2:  # Full Cloud
    stt: "openrouter"
    llm: "openrouter"    # or "router" with fleet[tier=cloud,lan]
    tts: "openrouter"
    system_prompt_tokens: 512
    timeout_s: 60

  3:  # TinkerClaw
    stt: "moonshine"
    llm: "tinkerclaw"
    tts: "piper"
    system_prompt_tokens: 256
    timeout_s: 180
```

Tab5 doesn't see this YAML; it just sends `voice_mode` (0/1/2/3) and Dragon picks the tuple.

## Mode-aware system prompts

Each mode gets a different system-prompt budget:

- **Local** — concise, 128 tokens. Small local models can't follow long instructions.
- **Hybrid** — medium, 256 tokens.
- **Cloud** — rich, 512 tokens. Frontier models can handle nuanced instruction.
- **TinkerClaw** — medium, 256 tokens. The gateway prepends its own agentic system prompt.

Override the prompts in `dragon_voice/config.yaml` → `voice.system_prompts.<mode>`.

## Mode-aware timeouts

The pipeline timeout is per-mode because local models routinely take 60–90 s for a tool-calling chain on slow hardware. Cloud is faster.

- **Local: 300 s** — covers slow Ollama on the ARM64 CPU
- **Hybrid: 60 s** — STT/TTS are fast; LLM is local-but-cached-warm
- **Cloud: 60 s** — should be plenty
- **TinkerClaw: 180 s** — gateway tasks can have tool-execution gaps

If you're on faster hardware, drop these. If you're on slower hardware (Pi 5, etc.), bump them.

## STT backends

| Backend | Notes |
|---------|-------|
| `moonshine` | Default local. ~0.3 s for a 10-second clip on Dragon. Tiny model size, decent accuracy. |
| `whisper_cpp` | Optional. Better accuracy, slower. Pick a model size in config. |
| `vosk` | Lightweight Russian/multilingual. |
| `openrouter` | Cloud — `gpt-audio-mini` via OpenRouter. Best quality + speed if you don't mind paying. |

## TTS backends

| Backend | Notes |
|---------|-------|
| `piper` | Default local. ONNX-based, multiple voices. 22050 Hz output, resampled to 16 kHz. |
| `kokoro` | Optional. Higher-quality local. Slower. |
| `edge_tts` | Microsoft's free TTS over their public API. Fast but cloud-dependent. |
| `openrouter` | Same as STT — `gpt-audio-mini`. 24 kHz output, resampled to 16 kHz. |

## API key auto-propagation

When you set `llm.openrouter_api_key`, it's auto-propagated to `stt.openrouter_api_key` and `tts.openrouter_api_key`. You don't have to set it three times.

## Validation before swap

When Tab5 sends a `config_update` requesting Cloud mode, Dragon validates:

1. `OPENROUTER_API_KEY` is set + non-empty
2. The selected model is in the OpenRouter catalog (or in the local fleet)
3. The cloud STT + TTS backends can be initialised

If any check fails, the swap is rejected — Dragon keeps the previous mode and emits an `error` event. Tab5 reverts to whatever was working.
