---
title: LLM backends
sidebar_label: LLM backends
---

# LLM backends

Dragon supports five concrete LLM backend types plus a meta-router that picks among them. All implement the same `LLMBackend` ABC in `dragon_voice/llm/base.py`.

## The five concrete backends

### `ollama` — local CPU inference

```yaml
llm:
  backend: "ollama"
  ollama_model: "ministral-3:3b"
  ollama_url: "http://localhost:11434"
```

Pulls models via `ollama pull <name>`. Default is **`ministral-3:3b`** (~65 s median, 7/10 correct-tool fires on the 10-prompt gauntlet). Other tested models in the [local LLM benchmarks table](https://github.com/lorcan35/TinkerBox/blob/main/CLAUDE.md#local-llm-benchmarks).

Multi-modal: detects models containing `llava` / `minicpm-v` / `minicpm-o` / `moondream` / `qwen2-vl` / `pixtral` / `internvl` substrings and declares VISION capability automatically.

### `openrouter` — cloud aggregator

```yaml
llm:
  backend: "openrouter"
  openrouter_api_key: "${OPENROUTER_API_KEY}"
  openrouter_model: "anthropic/claude-3.5-haiku"
```

Single account, 35+ models tracked in `_OPENROUTER_CAPS` registry. Capability declarations + pricing baked in. Streaming via standard OpenAI-format chat completions.

### `lmstudio` — local OpenAI-compatible server

```yaml
llm:
  backend: "lmstudio"
  lmstudio_url: "http://workstation.local:1234/v1"
  lmstudio_model: "openbmb/minicpm-v-4"
```

For a workstation on the same LAN running LM Studio's server. Unlike `ollama` (which is bound to localhost), LM Studio happily serves on the LAN.

Useful when: Dragon is a small SBC and you have a beefier machine elsewhere. Use it as the LAN-tier backend in fleet config.

### `npu_genie` — Qualcomm NPU

```yaml
llm:
  backend: "npu_genie"
  npu_genie_model_path: "/usr/share/qnn-htp/llama32.bin"
```

Runs Llama 3.2 1B on the QCS6490 HTP at ~8 tok/s. Text-only (QAIRT has no vision support yet). See [`docs/npu-setup.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/npu-setup.md) for setup.

This is the *fast* local mode. ~30× faster than ARM64 CPU Ollama.

### `tinkerclaw` — agentic gateway

```yaml
llm:
  backend: "tinkerclaw"
  tinkerclaw_url: "http://localhost:18789"
  tinkerclaw_token: "${TINKERCLAW_TOKEN}"
```

Routes through the [TinkerClaw Gateway](https://github.com/lorcan35/openclaw) running on the same Dragon. The gateway runs autonomous multi-step tasks — browsing the web, running shell commands, calling other skills. Use this for `voice_mode=3`.

## Bonus: `dual`

```yaml
llm:
  backend: "dual"
  dual_picker: "xLAM-2-1b-fc-r"
  dual_responder: "ministral-3:3b"
```

Two-backend pipeline: the *picker* model decides which tool (if any) to fire; the *responder* model writes the user-visible reply with the tool result in context. Picks tools fast (24 s median); responses are warm.

**Warning:** doesn't fit comfortably in 11 GB. Recommended for ≥16 GB hardware only. The Dragon Q6A's RAM gets evicted under sustained load. See `docs/PLAN-dual-model-pipeline.md` for the validation matrix.

## The router (meta-backend)

```yaml
llm:
  backend: "router"
  fleet:
    - id: ministral
      backend: ollama
      model_id: "ministral-3:3b"
      caps: [text, tool_calling]
      tier: local
      priority: 0
    - id: minicpm_v4
      backend: ollama
      model_id: "hf.co/openbmb/MiniCPM-V-4-gguf:Q4_K_M"
      caps: [text, vision, video]
      tier: local
      priority: 10
    # ... add as many as you want, mix tiers
```

The `CapabilityAwareRouter` picks per-turn based on:

1. What modalities the message contains (image_url → +VISION, video_url → +VIDEO, etc.)
2. The active `voice_mode`'s tier filter (Local mode = local tier only; Cloud mode = cloud + LAN)
3. The model with the lowest `priority` that satisfies the cap requirement

See [`docs/router-cookbook.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/router-cookbook.md) for full recipes (text-only-cheap, vision-on-budget, video-only-cloud, etc.).

## Hot-swap

Tab5 sends `{"type":"config_update","voice_mode":2,"llm_model":"anthropic/claude-sonnet-4.6"}` over WebSocket. Dragon hot-swaps the backend instance — no restart. Failed swaps roll back to the previous backend cleanly.
