---
title: WebSocket protocol
sidebar_label: WebSocket protocol
---

# WebSocket protocol

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

The full Tab5 ↔ Dragon wire format. Tab5 implements the **client side**; Dragon's `dragon_voice/server.py` is the server. Single source of truth: [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md). This page mirrors the canonical contract for cross-reference.

## Connection

```
ws://<dragon-host>:3502/ws/voice
```

Optional bearer auth header on connect (gated in production deployments).

On open, Tab5 sends a `register` frame; Dragon responds with `session_start.config`. Then the conversation proceeds.

## Tab5 → Dragon (sending)

| Type | When | Payload |
|------|------|---------|
| `register` | On WS connect | `{"device_id":"...", "session_id":"...", "capabilities":{...}}` |
| `start` | Mic press | `{"type":"start"}` (optional `"mode":"dictate"`) |
| `stop` | Mic release | `{"type":"stop"}` |
| `cancel` | User taps × | `{"type":"cancel"}` |
| `text` | Text input from chat | `{"type":"text", "content":"..."}` |
| `user_media` | Camera photo + chat | `{"type":"user_media", "media_id":"...", "text":"..."}` |
| `config_update` | Mode picker | `{"type":"config_update", "voice_mode":0\|1\|2\|3, "llm_model":"..."}` |
| `clear` | New chat button | `{"type":"clear"}` |
| `ping` | Every 15s during processing | `{"type":"ping"}` |

Plus binary frames:

- **Untagged binary** = raw 16 kHz mono int16 PCM (legacy mic-uplink path; routes to STT)
- **`VID0`-tagged binary** = JPEG video frame for the call relay
- **`AUD0`-tagged binary** = call audio (16 kHz PCM, *not* STT-bound)

## Dragon → Tab5 (receiving)

### Lifecycle

| Type | When | Payload |
|------|------|---------|
| `session_start` | After register | `{"session_id":"...", "config":{"backend":..., "fleet_summary":{...}, ...}}` |
| `config_update` | After Tab5 sends config_update | ACK with applied backend config + cloud_mode state + optional `error` field |
| `pong` | After Tab5 ping | Keepalive reply |
| `error` | Anything went wrong | `{"type":"error", "code":"...", "message":"..."}` |

### STT + LLM streaming

| Type | When | Payload |
|------|------|---------|
| `stt` | STT complete | `{"text":"..."}` |
| `stt_partial` | Dictation streaming | `{"text":"..." }` (incremental) |
| `llm` | Token streamed | `{"text":"...", "delta":"..."}` |
| `llm_done` | Stream complete | `{"llm_ms":234}` |
| `tool_call` | Tool invocation | `{"tool":"web_search", "args":{"query":"..."}}` |
| `tool_result` | Tool completed | `{"tool":"web_search", "result":{...}, "execution_ms":234}` |
| `dictation_summary` | After dictation post-process | `{"title":"...", "summary":"..."}` |

### TTS

| Type | When | Payload |
|------|------|---------|
| `tts_start` | Beginning of audio | `{"sample_rate":16000, "format":"pcm16"}` |
| (binary) | Mid-stream | Raw 16 kHz int16 PCM bytes |
| `tts_end` | End of audio | (no payload) |

### Rich media

| Type | When | Payload |
|------|------|---------|
| `media` | Code/table/image rendered | `{"media_type":"image", "url":"/api/media/abc.jpg", "width":660, "height":400, "alt":"..."}` |
| `card` | Rich preview card | `{"title":"...", "subtitle":"...", "image_url":"...", "description":"..."}` |
| `audio_clip` | Inline audio | `{"url":"...", "duration_s":2.3, "label":"..."}` |
| `text_update` | Replace last AI bubble | `{"text":"cleaned text"}` (Dragon strips rendered code blocks from the original text) |

### Widgets

| Type | When | Payload |
|------|------|---------|
| `widget_live` | Skill emits live state | `{"id":"...", "title":"...", "body":"...", "tone":"calm\|active\|urgent\|done", "icon":"...", "priority":N}` |
| `widget_live_update` | Live widget tick | `{"id":"...", "body":"...", "progress":0.7}` |
| `widget_live_dismiss` | Live widget done | `{"id":"..."}` |
| `widget_card` | One-shot card | `{"id":"...", "title":"...", "body":"...", "tone":"..."}` |
| `widget_list` | Scrollable list widget | `{"id":"...", "title":"...", "items":[...]}` |
| `widget_chart` | Line/bar chart | `{"id":"...", "title":"...", "data":[...]}` |
| `widget_prompt` | User-input request | `{"id":"...", "kind":"multiple_choice", "options":[...]}` |
| `widget_dismiss` | Generic remove | `{"id":"..."}` |

Tab5 → Dragon widget responses:

| Type | When | Payload |
|------|------|---------|
| `widget_action` | User tapped a widget button | `{"widget_id":"...", "action":"...", "value":"..."}` |
| `widget_capability` | Tab5 advertises supported widget types | `{"types":["live","card","prompt"]}` |

## Binary frame magic table

```
+--------+--------+----------------------+
| Magic  | Length | Payload              |
+--------+--------+----------------------+
| (none) | (none) | 16 kHz mono PCM      |  ← legacy mic uplink → STT
| VID0   | u32 BE | JPEG bytes            |  ← video call frame
| AUD0   | u32 BE | 16 kHz mono PCM       |  ← call audio (not STT)
+--------+--------+----------------------+
```

## Voice modes coverage

| Tab5 vmode | Sent on the wire | Notes |
|------------|------------------|-------|
| 0 Local | `voice_mode: 0` | Default mode |
| 1 Hybrid | `voice_mode: 1` | Cloud STT/TTS, local LLM |
| 2 Cloud | `voice_mode: 2`, `llm_model: "..."` | User-selected cloud model |
| 3 TinkerClaw | `voice_mode: 3` | Gateway routes |
| 4 Onboard | (not sent) | Tab5-side only — never crosses the wire. Dragon's ACK echoes are filtered out. |

## Backward compat

The legacy `cloud_mode` boolean still works (maps to voice_mode 0 or 2). The legacy `vision_capability` event still fires for old Tab5 firmware that doesn't read `fleet_summary`.
