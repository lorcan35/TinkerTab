---
audience: integrator
type: reference
prerequisites: none
last-verified: 2026-05-29
---
# WebSocket protocol (Tab5 client side)

Authoritative, lookup-oriented reference for the messages the
[Tab5](../../GLOSSARY.md) sends and receives over its single
[WebSocket](../../GLOSSARY.md) to the [Dragon](../../GLOSSARY.md). No tutorials
here.

The Tab5 holds **one** persistent WebSocket at `ws://<host>:3502/ws/voice`
(falling back to `wss://tinkerclaw-voice.ngrok.dev:443`). All traffic —
voice, text, vision, video, config, and channel messaging — multiplexes over
it. The **canonical full spec is the Dragon's**
[`docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md);
this page documents the **client side** that the Tab5 implements.

## Frame kinds

There are two frame kinds on the wire:

- **Text frames** — UTF-8 JSON with a `"type"` field.
- **Binary frames** — either untagged raw PCM, or an 8-byte-prefixed tagged
  payload.

> **WS send gotcha.** ESP-IDF's WS transport masks frames **in place** — never
> pass a string literal to `esp_transport_ws_send_raw()`; copy to a mutable
> buffer first.

## Binary frame magic tags

Non-audio binary payloads carry an 8-byte prefix: a 4-byte magic + a 4-byte
big-endian length, then `payload[len]`. **Untagged** binary frames are raw
16 kHz mono int16 PCM routed straight into STT — the legacy mic path, which
stays magic-less for backward compatibility.

| Magic | Direction | Length field | Payload | Module |
|---|---|---|---|---|
| `VID0` | both | 4 bytes BE u32 | JPEG frame | `voice_video.c` — camera uplink + downlink playback |
| `AUD0` | both | 4 bytes BE u32 | raw 16 kHz mono int16 PCM | `voice.c` call mode — `VOICE_MODE_CALL` wraps mic frames as `AUD0` (bypasses STT) |
| _(none)_ | Tab5 → Dragon | — | raw 16 kHz mono int16 PCM | legacy mic path → STT |

Wire layout: `"VID0"` (4 bytes) + `len_be` (4 bytes) + `payload[len]`. Same
shape for `"AUD0"`.

## Tab5 → Dragon (sending)

| `type` | Payload / shape | Purpose |
|---|---|---|
| `register` | `{"device_id":"...","session_id":"..."}` | First frame on connect — identifies the device + resumes a session. |
| `start` | optional `{"mode":"dictate"}` | Begin a voice/dictation turn; followed by untagged PCM frames. |
| `stop` | — | End the audio stream for the current turn. |
| `cancel` | — | Abort current processing. |
| `ping` | — | JSON keepalive heartbeat (every ~15 s during processing). |
| `text` | `{"content":"..."}` | A typed turn — skips STT, same conversation context. |
| `config_update` | `{"voice_mode":0..3,"llm_model":"..."}` | Swap [voice mode](../../GLOSSARY.md) / model. `cloud_mode` bool still accepted for backward compat. |
| `clear` | — | Reset conversation context. (Was documented as `clear_history`; the implementation has always used `clear`.) |
| `channel_reply` | `{"channel":"telegram","thread_id":"<id>","text":"<reply>"}` | Reply to a received `channel_message`; Dragon ACKs with `channel_reply_ack`. |

**Mode downconversion.** `config_update.voice_mode` is always in `0..3` on the
wire. Modes **4** (Onboard / K144) and **5** (Solo Direct) are Tab5-side-only:
the Tab5 downconverts them to `0` so the Dragon never sees them, and `voice.c`
filters the Dragon's ACK echo so local [NVS](../../GLOSSARY.md) (`vmode`) keeps
the true 4/5 value. This is a protocol feature — the on-the-wire shape stays
`0..3` as the Tab5 grows new local-only modes.

## Dragon → Tab5 (receiving)

| `type` | Shape / notes | Purpose |
|---|---|---|
| `session_start` | `session_id` | Session id for NVS persistence. |
| `stt` / `stt_partial` | transcription text | Final / streaming transcription (partial for dictation). |
| `llm` | streamed text | LLM response text. |
| `llm_done` | timing | LLM generation complete. |
| `tool_call` | `{"tool":"web_search","args":{...}}` | Tool invocation — show activity indicator. |
| `tool_result` | `{"tool":"...","result":{...},"execution_ms":N}` | Tool completion. |
| `tts_start` / binary TTS / `tts_end` | — | TTS audio playback bracketing. |
| `dictation_summary` | `{"title":"...","summary":"..."}` | Post-processing of a dictation. |
| `pong` | — | Keepalive response. |
| `config_update` | applied config + `cloud_mode` + `fleet_summary` | ACK echoing the applied backend config. `fleet_summary` carries per-device vision capability used to enable/disable the vision chip. |
| `error` | error details | Error report. A transient `stt_empty` / `pipeline_failed` snaps `voice_state` back to READY/IDLE. |

### Rich media (Dragon → Tab5)

| `type` | Shape | Purpose |
|---|---|---|
| `media` | `{"media_type":"image","url":"...","width":N,"height":N,"alt":"..."}` | Inline rendered image bubble (Dragon renders code/tables to JPEG). |
| `card` | `{"title":"...","subtitle":"...","image_url":"...","description":"..."}` | Rich preview card (orange accent border). |
| `audio_clip` | `{"url":"...","duration_s":2.3,"label":"..."}` | Inline audio. |
| `text_update` | `{"text":"cleaned text"}` | Replace the last AI bubble's text (after stripping rendered code blocks). |

### Channel messaging (Dragon → Tab5)

| `type` | Shape | Purpose |
|---|---|---|
| `channel_message` | `{"channel":"tg","message_id":"...","thread_id":"...","sender":{"display_name":"...","starred":bool},"text":"...","preview":"...","priority":"low|normal|high","needs_reply":bool}` | Incoming third-party platform message (Telegram / WhatsApp / Discord / Slack / Signal / iMessage / Matrix / Email). Routed to toast or now-card; gated by `ch_*_on` NVS toggles. |
| `channel_reply_ack` | `{"channel":"telegram","thread_id":"...","ok":bool,"platform_message_id":"..."}` | Confirms a Tab5-sent `channel_reply` was delivered. On `ok=true` the Tab5 toasts "Replied via {channel}". |

### Widget platform (Dragon → Tab5)

Skills on the Dragon emit typed widget state the Tab5 renders opinionatedly:
`widget_live`, `widget_live_update`, `widget_live_dismiss`, `widget_card`,
`widget_list`, `widget_chart`, `widget_prompt`, `widget_dismiss`. The Tab5 sends
`widget_action` and `widget_capability` back. Unknown widget types are ignored
(forward-compat). See [`WIDGETS.md`](../WIDGETS.md).

## Examples

A registration + typed turn (Tab5 → Dragon), JSON text frames:

```json
{"type":"register","device_id":"a1b2c3d4e5f6","session_id":""}
{"type":"text","content":"What time is it?"}
```

A mode switch to Hybrid (Tab5 → Dragon) — note the wire value stays `0..3`:

```json
{"type":"config_update","voice_mode":1,"llm_model":"anthropic/claude-3.5-haiku"}
```

A `VID0` video frame on the wire (conceptual byte layout):

```
56 49 44 30   00 00 12 34   <0x1234 bytes of JPEG ...>
'V''I''D''0'  len = 4660
```

## See also

- Canonical wire spec: [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md)
- [Voice modes reference](voice-modes.md) · [Observability events](observability-events.md)
- [The voice pipeline](../explanation/the-voice-pipeline.md) ·
  [TinkerTab architecture](../explanation/architecture.md)
