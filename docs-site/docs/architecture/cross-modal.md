---
title: Cross-modal flow
sidebar_label: Cross-modal flow
---

# Cross-modal flow (vision turns)

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

How a photo from Tab5 becomes part of the conversation, and stays in context for follow-up text turns.

## The naive bug we used to have

Pre-router, `_handle_user_media` in `server.py` bypassed `ConversationEngine` entirely. It built a single-turn `[{role:user, content:[image,text]}]` array and called `llm.generate_stream_with_messages(messages)` directly. Three problems:

1. **The vision turn wasn't threaded into MessageStore.** Follow-up text turns had no idea a photo had been sent.
2. **Tools didn't fire on vision turns.** ConvEngine's tool dispatcher was skipped entirely.
3. **Memory facts weren't injected.** Same reason — ConvEngine builds the augmented system prompt.

The router work fixed all three by re-threading vision turns through `ConvEngine.respond()`. This page describes the post-fix flow.

## The flow today

```
Tab5 → POST /api/media/upload (JPEG bytes)
       ← {"media_id": "abc123"}

Tab5 → WS: {"type":"user_media", "media_id":"abc123", "text":"describe this"}

server._handle_user_media()
  ├─ MessageStore.add_message(
  │      role="user",
  │      content=[{"type":"image_url",...},{"type":"text",...}],
  │      media_id="abc123"
  │   )
  │   → stores with __mm__: marker in the content column
  │
  └─ ConversationEngine.respond(session_id, message)
       ├─ build_context(media_store=...)
       │   → reads recent messages
       │   → for each multimodal message, resolves media_id → file path
       │     → loads JPEG → base64 → image_url content array
       │   → returns OpenAI-format messages list
       │
       ├─ router.choose(required_caps={TEXT, VISION}, tier=local)
       │   → MiniCPM-V-4 selected (lowest priority + VISION cap)
       │
       ├─ ollama_llm.generate_stream_with_messages(messages)
       │   → translates OpenAI multimodal format to Ollama's flat
       │     content + images format
       │   → streams response tokens
       │
       └─ MessageStore.add(role=assistant, content=response)
```

## The follow-up text turn

```
Tab5 → WS: {"type":"text", "content":"what color was the chair?"}

server._handle_text()
  └─ ConversationEngine.respond(session_id, "what color was the chair?")
       ├─ build_context(media_store=...)
       │   → re-reads the recent messages including the prior multimodal user msg
       │   → re-hydrates the photo from MediaStore (same media_id)
       │   → context now includes: [system, photo+desc, response, "what color"]
       │
       ├─ router.choose(required_caps={TEXT}, tier=local)
       │   → router has VISION-capable model still in instances pool
       │   → chooses based on text-only requirement, but the model handles
       │     both modalities, so the photo stays in context
       │
       └─ LLM responds with knowledge of the photo: "The chair was oak."
```

The key is `MessageStore.get_context(media_store=...)` re-hydrating multimodal content arrays *every* turn, not just the turn the photo was attached to.

## Trim policy

Image content arrays cost more tokens than plain text. The trim heuristic counts a multimodal message as ~3× a text message. Older photos drop out of context first. When a photo file expires (24 h TTL by default), MessageStore strips its content array and substitutes a `[image expired]` placeholder so the conversation thread doesn't break.

## Tool calling on vision turns

If the active fleet includes a model with both VISION and TOOL_CALLING (Claude Haiku, GPT-4o, Gemini Flash), the router prefers it for vision turns where the LLM might need to call a tool — like asking "translate the text on this sign". If only a vision-only model is available (MiniCPM-V lacks tool training), tools are skipped that turn and an `tools_unsupported_for_modality` info event is emitted.

## OllamaBackend's hidden translator

OpenAI-format multimodal content arrays look like:

```json
[
  {"type":"image_url", "image_url":{"url":"data:image/jpeg;base64,/9j..."}},
  {"type":"text", "text":"describe this"}
]
```

Ollama's API expects:

```json
{"content":"describe this", "images":["base64..."]}
```

`OllamaBackend.generate_stream_with_messages` does the translation. Without it, router-fed vision turns failed with `json: cannot unmarshal array into Go struct field`.

## fleet_summary in protocol

`session_start.config.fleet_summary` includes per-modality the model_id the router would pick at the active tier:

```json
{
  "fleet_summary": {
    "text":     "ministral-3:3b",
    "vision":   "hf.co/openbmb/MiniCPM-V-4-gguf:Q4_K_M",
    "video":    "hf.co/openbmb/MiniCPM-V-4-gguf:Q4_K_M",
    "audio_in": null,
    "audio_out": null,
    "tool_calling": "ministral-3:3b"
  }
}
```

Tab5 firmware uses this to light up vision/video/audio capability chips dynamically. Older firmware ignores the field and falls back to the legacy `vision_capability` event.
