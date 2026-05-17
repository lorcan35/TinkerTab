---
title: Module map
sidebar_label: Module map
---

# Module map

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Where each Dragon-side concern lives. The whole repo is at [github.com/lorcan35/TinkerBox](https://github.com/lorcan35/TinkerBox).

## Top-level

```
TinkerBox/
├── dragon_voice/        Voice pipeline package (port 3502)
├── dashboard.py         Web dashboard (port 3500)
├── schema.sql           SQLite schema (11 tables)
├── systemd/             Service unit files
├── docs/                ARCHITECTURE.md, protocol.md, dev-setup.md, etc.
├── tests/               69 test_*.py files (~556 in CI)
└── requirements.txt     Python deps
```

## `dragon_voice/` package

```
dragon_voice/
├── __main__.py             python3 -m dragon_voice entry
├── server.py               WS handler family + middleware wiring
├── pipeline.py             STT → LLM → TTS orchestration
├── conversation.py         Multi-turn engine
├── sessions.py             Lifecycle: create / pause / resume / end
├── messages.py             Append-only MessageStore
├── db.py                   aiosqlite layer (WAL mode)
├── memory.py               Facts + documents + RAG
├── config.py               Dataclasses + validators
├── config.yaml             Default config
│
├── middleware/             Request filters (each stateless, explicit deps)
│   ├── cors.py
│   ├── security_headers.py
│   ├── auth.py             Bearer-token gate
│   └── rate_limit.py
│
├── handlers/               HTTP endpoint handlers
│   ├── debug.py            tracemalloc / RSS / GC / widget emitters
│   ├── status.py           HTML status + JSON liveness
│   └── config_api.py       Hot-reload + pipeline backend swap
│
├── lifecycle/              Boot / shutdown / monitors / cleanup
│   ├── monitors.py         5-min RSS/temp/FD sample
│   ├── purge.py            Message retention + media cleanup
│   ├── startup.py          DB → sessions → memory → conversation → REST
│   └── shutdown.py         Drain pipelines → release backends → close clients
│
├── api/                    Modular REST API (54 endpoints)
│   ├── __init__.py         setup_all_routes()
│   ├── sessions.py         /api/v1/sessions/*
│   ├── messages.py         /api/v1/messages/*
│   ├── devices.py          /api/v1/devices/*
│   ├── config_routes.py    /api/v1/config/*
│   ├── events.py           /api/v1/events
│   ├── agent_log.py        /api/v1/agent_log
│   ├── synthesize.py       /api/v1/transcribe + /synthesize + OTA
│   ├── completions.py      /api/v1/completions (stateless)
│   ├── system.py           /api/v1/system + backends
│   ├── tools.py            /api/v1/tools/*
│   ├── memory_routes.py    /api/v1/memory/*
│   ├── documents.py        /api/v1/documents/*
│   └── media_routes.py     /api/media/{id} + /api/media/upload
│
├── tools/                  Skill registry + 13 built-in tools
│   ├── base.py             Tool ABC
│   ├── registry.py         Parser + executor + agent_log feed
│   ├── response_wrap.py    Empty-reply guard
│   ├── web_search.py       SearXNG (DDG fallback)
│   ├── memory_tools.py     remember / recall / forget
│   ├── datetime_tool.py
│   ├── calculator_tool.py
│   ├── unit_converter_tool.py
│   ├── weather_tool.py
│   ├── system_tool.py
│   ├── stock_ticker_tool.py
│   ├── timesense_tool.py   Pomodoro + widget_live emitter
│   ├── quick_poll_tool.py  Widget-skill reference example
│   └── note_tool.py
│
├── stt/                    Speech-to-text backends
├── tts/                    Text-to-speech backends
├── llm/                    LLM backends + multi-model router
│   ├── base.py             LLMBackend ABC + Modality enum
│   ├── router.py           CapabilityAwareRouter, ModelSpec, TIER_FOR_MODE
│   ├── ollama_llm.py       Local + multimodal-aware translator
│   ├── openrouter_llm.py   Cloud + 35-model capability registry
│   ├── lmstudio_llm.py     LAN OpenAI-compat
│   ├── npu_genie.py        QAIRT/Genie text-only
│   ├── tinkerclaw_llm.py   Gateway adapter
│   └── dual.py             Two-backend picker+responder
│
├── notes/                  Notes module (CRUD + search + audio)
├── media/                  Rich-media rendering
│   ├── store.py            Disk-backed file storage, 24h cleanup
│   ├── pipeline.py         Code/table/image-URL detection + JPEG render
│   └── url_signer.py       HMAC-signed time-bounded URLs
│
├── scheduler/              In-process scheduler + sqlite store
│   ├── manager.py          SchedulerManager + REST glue
│   ├── models.py           Notification dataclass
│   ├── parser.py           Natural-language time parser
│   └── store.py            InMemory + Sqlite stores
│
├── surfaces/               Tab5 widget-surface abstraction
└── mcp/                    Model Context Protocol client + bridge
```

## Service map (process-level)

| Service | Port | Unit | Owner |
|---------|------|------|-------|
| Dashboard | 3500 | tinkerclaw-dashboard | dashboard.py |
| Voice | 3502 | tinkerclaw-voice | dragon_voice |
| mDNS | — | tinkerclaw-mdns | avahi service file |
| Ollama | 11434 | ollama | external |
| SearXNG | 8888 | searxng | external (Docker) |
| TinkerClaw GW | 18789 | tinkerclaw-gateway | OpenClaw sidecar (optional) |
| ngrok | 443 (ext) | tinkerclaw-ngrok | tunnels for the three above |

## Database (`schema.sql`)

11 tables:

- **Foundation (6)** — `devices`, `sessions`, `messages`, `notes`, `events`, `config`
- **Memory (3)** — `memory_facts`, `memory_documents`, `memory_chunks`
- **Scheduler (2)** — `scheduled_notifications`, `notification_queue`

Async access via aiosqlite in WAL mode. Single `db.py` module — no raw SQL scattered across files.

## Tests

- **Named CI set** — runs without a live server (~33 files)
- **Local-only** — `test_api_e2e.py` (29 scenarios), `test_e2e_dragon.py` (live Dragon)
- **Aggregate** — 556 tests pass when running `pytest tests/ -q --ignore=tests/audit`

## Refactor history note

The pre-2026-04-24 monolithic `server.py` (2,747 LOC) was decomposed into the four sibling packages above (`middleware/` / `handlers/` / `lifecycle/` / slimmed `server.py`). File:line citations from older audit docs point at the pre-decomposition file — many of those positions now live elsewhere.
