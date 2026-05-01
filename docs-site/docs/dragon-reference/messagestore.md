---
title: MessageStore + media
sidebar_label: MessageStore + media
---

# MessageStore + media handling

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Where conversation history lives, and how multimodal content survives the round-trip.

## The MessageStore

`dragon_voice/messages.py`. SQLite-backed, append-only. Every Tab5 turn writes a user message + an assistant response (+ optional tool messages in between).

```python
class MessageStore:
    async def add_message(
        self,
        session_id: str,
        role: str,              # "user" | "assistant" | "system" | "tool"
        content: str | list,    # str for plain text; list for OpenAI-format multimodal
        media_id: str | None = None,
        metadata: dict | None = None,
    ) -> Message: ...

    async def get_context(
        self,
        session_id: str,
        media_store: MediaStore | None = None,
        max_tokens: int = 4000,
    ) -> list[dict]: ...
```

## Append-only

There is **no** API to mutate an existing message. Corrections are new messages with metadata indicating they correct a previous one. This makes the conversation thread trivially auditable + reproducible.

## Multimodal storage

Vision turns persist with the OpenAI-format `content` array — a list with `image_url` + `text` items:

```python
content = [
    {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,..."}},
    {"type": "text", "text": "describe this"}
]
```

But we don't store the giant base64 inline. Instead:

1. The image is uploaded via `POST /api/media/upload` and assigned a `media_id`
2. `MessageStore.add_message(content=..., media_id="abc123")` stores a JSON marker:

```
__mm__:[{"type":"image_url","media_id":"abc123"},{"type":"text","text":"..."}]
```

3. On context build, `media_store.resolve(media_id)` reads the JPEG from disk + base64-encodes it back into the content array

This way the SQLite row is small (just the marker), even for conversations with many photos.

## `get_context()` rehydration

`MessageStore.get_context(session_id, media_store=...)` walks recent messages, hydrates multimodal content arrays from disk, and returns a flat OpenAI-format messages list ready for `LLMBackend.generate_stream_with_messages(messages)`.

This is the **only** place messages get hydrated. The router doesn't reach into the store; it gets a complete messages list from `ConversationEngine`. Backends don't reach into the store either — they get the messages list verbatim.

## Trim policy

When the context exceeds `max_tokens`, oldest messages drop first. Image content arrays count ~3× the cost of plain text — a heuristic that matches typical vision-model token counting.

## Media TTL

`MediaStore` cleans up files older than 24 hours (configurable). When a message references a `media_id` whose file has expired:

1. `media_store.resolve(media_id)` returns None
2. `get_context` substitutes a `[image expired]` text placeholder
3. The conversation thread continues; just the image is gone

This is intentional — the alternative (failing the turn entirely) is worse. The user might be asking a follow-up where the image isn't critical.

## Media upload

`POST /api/media/upload` accepts BMP or JPEG, converts + resizes via Pillow (max 660 px wide), JPEG-encodes at quality 80, stores at `~/.tinkerclaw/media/<media_id>.jpg`, returns `{"media_id": "..."}`.

Tab5's camera screen uses this for photo uploads. The dashboard uses it for image-by-paste in the chat tab. The MCP bridge uses it for media-bearing tool results.

## Media serving

`GET /api/media/{id}` streams the file with `Cache-Control: max-age=3600`. URLs are HMAC-signed + time-bounded via `media/url_signer.py` so an unauthenticated browser tab in the rich-media chat overlay can load the image without leaking the auth token.

## On-disk layout

```
~/.tinkerclaw/
├── dragon.db                     SQLite — sessions, messages, memory, scheduler, devices
└── media/
    ├── abc123.jpg                Tab5 camera upload
    ├── def456.jpg                LLM-rendered code block as JPEG
    └── ...
```

## Backups

Two paths:

```bash
cp ~/.tinkerclaw/dragon.db ~/backup-dragon-$(date +%Y%m%d).db
tar czf ~/backup-media-$(date +%Y%m%d).tgz -C ~/.tinkerclaw media
```

Restore: stop `tinkerclaw-voice`, replace files, restart. The database is the entire state — there's no in-memory cache that needs separate persistence.
