---
title: Photos & vision
sidebar_label: Photos & vision
---

# Photos & vision

The Tab5 has a 2 MP camera. Send any frame to the LLM, get analysis back, ask follow-up text questions referencing the same image. This page covers the user-facing flow.

## The 30-second flow

1. From the **chat overlay**, tap the **camera icon** in the input bar
2. The camera screen opens; frame your shot
3. Tap **Send photo**
4. Wait ~3-8 seconds (cloud) or ~30-60 seconds (local) for the LLM's analysis
5. Ask follow-up: *"what's the wood grain on the chair?"*

The follow-up turn still has the photo in context. Cross-modal continuity *just works* — see the [cross-modal architecture](/docs/architecture/cross-modal) for how.

## Which model is doing the looking?

Depends on your voice mode. The Dragon multi-model router picks per-turn:

| Voice mode | Vision model picked |
|------------|---------------------|
| Local (0) | `MiniCPM-V-4` (Ollama, local NPU) |
| Hybrid (1) | (no vision in Hybrid; falls back to Local for vision turns) |
| Full Cloud (2) | `qwen3.6-flash` for cheap, `claude-sonnet-4.6` for quality, `gemini-3-flash` for native video |
| TinkerClaw (3) | gateway picks (Claude Haiku default) |
| Onboard (4) | not supported — no vision model on K144 v1.3 |

If your active mode has no vision-capable model, the camera button is greyed out and the chat shows a `vision_unsupported` banner.

The `fleet_summary.vision` field in `session_start.config` tells Tab5 which model would be picked. Tab5 uses this to render capability chips dynamically.

## Cross-modal continuity

```
turn 1: [photo of chair] + "describe this"
        → router picks vision model
        → photo persisted in MessageStore as multimodal content array
        → LLM: "A wooden chair, oak frame, blue cushion."

turn 2: "what color was the cushion?"
        → user message is text-only, but MessageStore hydrates the photo
          back into context from turn 1
        → router picks the same vision model (still has VISION cap)
        → LLM: "Blue."

turn 3: "and the frame?"
        → same flow
        → LLM: "Oak."
```

The photo stays in context for as long as the conversation does. Trim policy is oldest-first; image content arrays count as a heavier token cost than plain text, so they age out faster.

## Photo storage on Dragon

When Tab5 uploads a photo, it lands in `~/.tinkerclaw/media/` with a `media_id`. Default TTL is **24 hours** — after that the file is purged and any context referencing it is replaced with `[image expired]`.

You can extend the TTL in Dragon's `config.yaml` → `media.ttl_h` (default 24).

## Photo storage on Tab5

Captures saved to `/sdcard/IMG_NNNN.jpg`. They stay until you delete them. The auto-incrementing counter survives reboots (resumed from existing files at boot).

## Bypassing the chat (just see what's there)

If you just want the LLM to describe what it sees without needing to capture:

1. Open the camera screen directly (nav sheet → Camera)
2. Tap the chat-bubble button at the top
3. The current frame is captured + uploaded
4. The chat overlay opens with the photo pre-attached

## Privacy

The photo is sent over WebSocket to Dragon. In Local mode it stays on Dragon. In Cloud mode it's forwarded to OpenRouter as base64-encoded multipart content.

If you'd rather *never* send photos to a cloud, set Settings → Voice → mode to Local and don't change it. The router will refuse to pick cloud vision models.
