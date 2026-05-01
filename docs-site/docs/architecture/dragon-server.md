---
title: Dragon server overview
sidebar_label: Dragon server overview
---

# Dragon server overview

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

The Dragon Q6A server (`tinkerclaw-voice` systemd unit, port 3502) is the **brain** half. Python 3.12, FastAPI/aiohttp WebSocket server, SQLite storage, ~54 REST endpoints, 11-tab dashboard SPA on port 3500. Hosts STT + LLM + TTS + memory + skills + scheduler + multi-model router.

## Module layout

```
dragon_voice/
├── server.py             ~2,700 LOC — WS-voice handler family
├── pipeline.py           STT → LLM → TTS orchestration + VAD + dictation
├── conversation.py       Multi-turn ConversationEngine (tools + memory injection)
├── sessions.py           Lifecycle: create / pause / resume / end
├── messages.py           Append-only MessageStore (multimodal aware)
├── db.py                 Async aiosqlite layer (WAL mode)
├── memory.py             MemoryService: facts + documents + RAG
├── config.py             Config dataclasses + validators
├── config.yaml           Default configuration
├── middleware/           CORS / security headers / auth / rate limit
├── handlers/             HTTP endpoint handlers (debug / status / config_api)
├── lifecycle/            Boot / shutdown / monitors / cleanup
├── api/                  Modular REST API (54 endpoints)
├── tools/                Skill registry + 10 built-in tools
├── stt/                  Backends: moonshine, whisper_cpp, vosk, openrouter
├── tts/                  Backends: piper, kokoro, edge_tts, openrouter
├── llm/                  Backends: ollama, openrouter, lmstudio, npu_genie,
│                                   tinkerclaw, dual + the multi-model router
├── notes/                Notes module (CRUD + search + audio ingestion)
├── media/                Rich-media pipeline (Pygments code blocks, tables)
├── scheduler/            In-process scheduler + sqlite-backed notification store
├── surfaces/             Tab5 widget-surface abstraction
└── mcp/                  Model Context Protocol client + bridge
```

## The three discipline boundaries

1. **Sessions are not connections.** Sessions survive disconnects. A device reconnect resumes the existing session. `SessionManager` handles the lifecycle.
2. **Conversation items are append-only.** Never mutate. New tool result → new message; correction → new message with metadata.
3. **Devices are first-class.** Registered with capabilities + tracked online/offline. Multi-Tab5 deployments work because each device has its own pipeline state.

## How a voice turn flows

```
1. Tab5 → WS frame: {"type":"start"}
2. Tab5 → WS frame: 16 kHz PCM bytes (raw, untagged)
3. Tab5 → WS frame: {"type":"stop"}
4. server.py → pipeline.process_voice_turn(audio)
5. pipeline → STT (Moonshine | OpenRouter)
6. pipeline → MessageStore.add(role=user, content=text)
7. pipeline → ConversationEngine.respond(session, message)
   ├─ build_context() injects memory facts + document chunks + active widgets
   ├─ LLM streams reply
   ├─ if tool marker: parse, execute, inject result, continue
   ├─ MessageStore.add(role=assistant, content=...)
8. pipeline → MediaPipeline.process_response(text) — renders code/tables
9. server.py → WS: stream "llm" + "media" + "tts_start" + binary TTS + "tts_end"
```

[Full trace →](/docs/architecture/voice-pipeline)

## Multi-model routing

When `llm.backend = "router"` in `config.yaml`, the `CapabilityAwareRouter` picks per-turn from a declared fleet. The active voice mode's tier filter constrains the candidate set:

```
TIER_FOR_MODE = {
    0: {"local"},          # Local
    1: {"local"},          # Hybrid
    2: {"cloud", "lan"},   # Full Cloud (LAN tier eligible)
    3: None,                 # TinkerClaw — bypass router
}
```

A vision turn (image_url in content) → +VISION required cap → router picks a vision-capable model in the active tier. Lowest priority wins.

[Router cookbook](https://github.com/lorcan35/TinkerBox/blob/main/docs/router-cookbook.md) has full recipes.

## Where state lives

| State | Backing |
|------|---------|
| Sessions, messages, devices, config | SQLite at `~/.tinkerclaw/dragon.db` |
| Memory facts, document chunks (with embeddings) | SQLite + sqlite-vec |
| Scheduled notifications | SQLite `scheduled_notifications` table |
| Media uploads (photos, audio) | Disk at `~/.tinkerclaw/media/` (24 h TTL) |
| In-flight WebSocket connections | In-process `ConnectionManager` |
| Active LLM backend instances | `BackendPool` (lazy + reference-counted) |

The whole user state is **two paths**: `~/.tinkerclaw/dragon.db` and `~/.tinkerclaw/media/`. Backups are `cp` + restart.

## Companion modules

- **[Cross-modal flow](/docs/architecture/cross-modal)** — how vision turns thread back into context
- **[Video calling stack](/docs/architecture/video-calling-stack)** — VID0/AUD0 broadcast relay
- **[Skill registry](/docs/architecture/skills)** — tool-call lifecycle + observability
