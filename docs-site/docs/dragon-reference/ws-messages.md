---
title: WS message catalog
sidebar_label: WS message catalog
---

# WS message catalog

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Full reference for every Tab5 ↔ Dragon WebSocket message, server-side perspective. The user-facing summary lives at [WebSocket protocol](/docs/firmware-reference/ws-protocol). This page is the implementer's reference.

Source of truth: [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md).

## Connection

```
ws://<dragon-host>:3502/ws/voice
```

## Message types — Tab5 → Dragon

### `register`

Sent on WS connect.

```json
{
  "type": "register",
  "device_id": "abcdef123456",
  "session_id": "...",        // optional, resumes existing session
  "capabilities": {
    "audio_codec": ["pcm", "opus"],
    "video_codec": ["jpeg"],
    "widgets": ["live", "card", "list", "chart", "media", "prompt"],
    "vmode_supported": [0, 1, 2, 3, 4]
  }
}
```

### `start`

Begin mic capture.

```json
{
  "type": "start",
  "mode": "dictate"        // optional; default "ask"
}
```

After this, Tab5 streams binary 16 kHz PCM frames until the next `stop`.

### `stop`

End mic capture. Triggers STT → LLM → TTS pipeline.

### `cancel`

Abort the in-flight turn. STT/LLM/TTS are killed; conversation context is preserved.

### `text`

Skip STT; send text directly to the LLM.

```json
{ "type": "text", "content": "what time is it?" }
```

### `user_media`

Vision turn. Image must have been pre-uploaded via `POST /api/media/upload`.

```json
{
  "type": "user_media",
  "media_id": "abc123",
  "text": "describe this"   // optional caption
}
```

### `config_update`

Switch voice mode + LLM model.

```json
{
  "type": "config_update",
  "voice_mode": 0,                          // 0..3
  "llm_model": "anthropic/claude-3.5-haiku" // optional, cloud-mode only
}
```

Backward compat: `cloud_mode: true` is also accepted (maps to voice_mode 0 or 2).

### `clear`

Wipe the current conversation. New chat button.

### `ping`

Keepalive. Sent every 15 s during slow inference (Tab5 won't time out the WS).

## Message types — Dragon → Tab5

### `session_start`

After register, Dragon responds with the active config.

```json
{
  "type": "session_start",
  "session_id": "...",
  "config": {
    "voice_mode": 0,
    "stt": "moonshine",
    "llm": "ollama:ministral-3:3b",
    "tts": "piper",
    "fleet_summary": {
      "text": "ministral-3:3b",
      "vision": "hf.co/openbmb/MiniCPM-V-4-gguf:Q4_K_M",
      "video": "hf.co/openbmb/MiniCPM-V-4-gguf:Q4_K_M",
      "audio_in": null,
      "audio_out": null,
      "tool_calling": "ministral-3:3b"
    }
  }
}
```

### `stt` / `stt_partial`

```json
{ "type": "stt", "text": "what time is it" }
{ "type": "stt_partial", "text": "what time" }
```

### `llm`

Streamed token. `delta` is incremental, `text` is the cumulative response so far.

```json
{ "type": "llm", "delta": "It's", "text": "It's" }
{ "type": "llm", "delta": " 3:42", "text": "It's 3:42" }
```

### `llm_done`

End of stream.

```json
{ "type": "llm_done", "llm_ms": 1234 }
```

### `tool_call` / `tool_result`

```json
{
  "type": "tool_call",
  "tool": "web_search",
  "args": { "query": "weather in Paris" }
}

{
  "type": "tool_result",
  "tool": "web_search",
  "result": { "snippets": [...] },
  "execution_ms": 234
}
```

### `tts_start` / binary / `tts_end`

```json
{
  "type": "tts_start",
  "sample_rate": 16000,
  "format": "pcm16"
}
```

Then binary frames (raw 16 kHz int16 PCM).

```json
{ "type": "tts_end" }
```

### `dictation_summary`

After dictation post-process.

```json
{
  "type": "dictation_summary",
  "title": "Shopping list",
  "summary": "Eggs, milk, bread, ..."
}
```

### `media`

Rich-media render.

```json
{
  "type": "media",
  "media_type": "image",
  "url": "/api/media/abc.jpg",
  "width": 660,
  "height": 400,
  "alt": "Code: python"
}
```

### `card` / `audio_clip`

```json
{
  "type": "card",
  "title": "...",
  "subtitle": "...",
  "image_url": "...",
  "description": "..."
}

{
  "type": "audio_clip",
  "url": "...",
  "duration_s": 2.3,
  "label": "..."
}
```

### `text_update`

Replace the last AI bubble's text (Dragon strips rendered code blocks from the original).

```json
{ "type": "text_update", "text": "cleaned text" }
```

### `widget_*`

See [WebSocket protocol overview](/docs/firmware-reference/ws-protocol#widgets) for the full widget vocabulary.

### `error`

```json
{
  "type": "error",
  "code": "modality_unsupported",
  "message": "Active fleet has no vision-capable model"
}
```

### `pong`

Keepalive reply.

## Binary frame magic

| Magic | Length field | Payload | Direction |
|-------|--------------|---------|-----------|
| (none) | (none) | 16 kHz mono PCM | Tab5 → Dragon (mic uplink, STT-bound) |
| `VID0` | u32 BE | JPEG | both (call relay) |
| `AUD0` | u32 BE | 16 kHz mono PCM | both (call audio, NOT STT) |
