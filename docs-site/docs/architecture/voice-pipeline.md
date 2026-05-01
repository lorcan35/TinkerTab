---
title: Voice pipeline
sidebar_label: Voice pipeline
---

# Voice pipeline

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

A complete trace of a single voice turn from Tab5's microphone to the speaker. Where modules live + what they own.

## End-to-end picture

```
Tab5                           Dragon                            Tab5
─────                          ──────                            ─────
mic → ES7210 → I2S RX
     → mic.c (3:1 downsample 48 → 16 kHz)
     → voice.c WS uplink ──── 16 kHz PCM ────→ server.py.handle_voice_audio
                                                  ↓
                                              pipeline._capture_audio (VAD)
                                              accumulates frames in ring
                                                  ↓
                                              {"type":"stop"} →
                                              pipeline._do_stt(audio)
                                                  ↓
                                              STT backend (Moonshine | OpenRouter)
                                                  ↓
                                              MessageStore.add(role=user, content)
                                                  ↓
                                              ConversationEngine.respond(session_id, text)
                                                  ├─ build_context(): memory, docs, widgets
                                                  ├─ LLM stream → tokens
                                                  ├─ tool parser: <tool>...</tool>
                                                  │     ↓
                                                  │   tool.execute() → result
                                                  │     ↓
                                                  │   continue stream with result in context
                                                  ├─ response_wrap (empty-reply guard)
                                                  └─ MessageStore.add(role=assistant)
                                                  ↓
                                              MediaPipeline.process_response()
                                                  → renders code/tables → JPEG → media events
                                                  ↓
                                              TTS backend (Piper | OpenRouter)
                                                  ↓
                                              16 kHz mono PCM stream ───→ voice.c
                                                                          ↓
                                                                      1:3 upsample
                                                                          ↓
                                                                      I2S TX → ES8388 → speaker
```

## Tab5 side

[`main/mic.c`](https://github.com/lorcan35/TinkerTab/blob/main/main/mic.c)
: ES7210 quad-mic in TDM mode. Pulls 48 kHz int16 frames, decimates 3:1 to 16 kHz, extracts slot 0 as the primary mic.

[`main/voice.c`](https://github.com/lorcan35/TinkerTab/blob/main/main/voice.c)
: WS state machine — connect, register, listen, dictate, cancel, reconnect. Handles the JSON event types + binary TTS frames. Five voice modes routed through here (modes 0-3 use Dragon WS; mode 4 routes via UART to K144).

[`main/voice_codec.{c,h}`](https://github.com/lorcan35/TinkerTab/blob/main/main/voice_codec.h)
: OPUS capability negotiation + (gated-on) encode/decode. Today the encoder runs at 24 kbps OPUS; decoder is ready for Dragon → Tab5 OPUS TTS once Dragon emits OPUS frames.

## Dragon side

`dragon_voice/pipeline.py`
: The orchestrator. Owns VAD (silero VAD plus an adaptive RMS gate for dictation), dictation buffering (64 KB), and the STT → LLM → TTS sequence. Exposes `process_voice_turn(audio)` and `process_text_turn(text)` symmetrically.

`dragon_voice/conversation.py`
: `ConversationEngine.respond(session_id, message)`. Builds the LLM context (system prompt + history + memory facts + document chunks + active widgets), streams tokens, parses tool markers, executes tools (max 3 per turn), threads results back, returns when the LLM emits a stop token.

`dragon_voice/messages.py`
: Append-only `MessageStore`. Persists the user message + assistant response + tool messages. Multimodal-aware — `image_url` content arrays are stored with a `__mm__:` JSON marker and re-hydrated from `MediaStore` on context build for cross-modal continuity.

`dragon_voice/tools/registry.py`
: Tool dispatcher. Parses three XML dialects (legacy, standard FC, bracketed-name). Empty-reply guard wraps a one-line ack from the tool result if the LLM emitted only a tool call. Every execution feeds the agent_log ring buffer.

`dragon_voice/media/pipeline.py`
: Post-LLM rich-media renderer. Detects code blocks, markdown tables, image URLs in the response. Renders code via Pygments → JPEG, tables via Pillow → JPEG. Emits `media` events on the WS so Tab5 can display them inline.

## Mode-aware variations

- **Local mode** — STT = Moonshine, LLM = Ollama (or NPU Genie or router-with-local-fleet), TTS = Piper. 60–90 s per turn.
- **Hybrid mode** — STT/TTS = OpenRouter `gpt-audio-mini`, LLM = local. 4–8 s per turn.
- **Full Cloud mode** — STT/TTS/LLM = OpenRouter. 3–6 s per turn.
- **TinkerClaw mode** — LLM call goes to the gateway agent runner; ConversationEngine + tools + memory all bypassed (the gateway handles its own).

## Auto-fallback

If a cloud STT or TTS call fails (timeout, API error), the pipeline auto-falls back to the local equivalent for that turn AND emits `config_update.error` → Tab5 reverts to Local mode automatically. Same pattern for the TinkerClaw gateway being down.
