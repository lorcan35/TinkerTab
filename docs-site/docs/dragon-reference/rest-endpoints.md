---
title: REST endpoints
sidebar_label: REST endpoints
---

# REST endpoints

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

54 endpoints across `/api/v1/*` and `/api/*`. All bearer-token-gated except discovery + health. The auth token comes from `~/.env` → `DRAGON_API_TOKEN`.

```bash
export TOKEN="$(cat /home/radxa/.env | grep DRAGON_API_TOKEN | cut -d= -f2)"
```

## Sessions (`/api/v1/sessions`)

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v1/sessions` | List sessions (filter by device, status) |
| POST | `/api/v1/sessions` | Create session |
| GET | `/api/v1/sessions/{id}` | Get session |
| POST | `/api/v1/sessions/{id}/end` | End session |
| POST | `/api/v1/sessions/{id}/resume` | Resume paused session |
| POST | `/api/v1/sessions/{id}/pause` | Pause active session |
| PATCH | `/api/v1/sessions/{id}` | Update title/system_prompt/metadata |
| GET | `/api/v1/sessions/{id}/context` | Get formatted LLM context |

## Messages

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v1/sessions/{id}/messages` | List messages (paginated) |
| POST | `/api/v1/sessions/{id}/chat` | SSE streaming LLM chat |
| GET | `/api/v1/messages/{id}` | Get single message |
| DELETE | `/api/v1/sessions/{id}/messages` | Purge session messages |

## Devices

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v1/devices` | List devices |
| GET | `/api/v1/devices/{id}` | Get device |
| PATCH | `/api/v1/devices/{id}` | Update device name/config |
| DELETE | `/api/v1/devices/{id}` | Remove device |

## Config (scoped: global / device / session)

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v1/config` | List config by scope |
| GET | `/api/v1/config/{key}` | Get config (with scope resolution) |
| PUT | `/api/v1/config/{key}` | Set config value |
| DELETE | `/api/v1/config/{key}` | Delete config key |

Resolution order: session > device > global. More specific wins.

## Events + agent log

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v1/events` | List events (filter by type/session/device) |
| GET | `/api/v1/agent_log` | Cross-session tool-call activity feed (last 64) |

The agent_log feed is populated at the `ToolRegistry.execute` chokepoint — every tool execution from any caller (WS, REST, dashboard) shows up here.

## Media

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/api/v1/transcribe` | STT: audio bytes → text |
| POST | `/api/v1/synthesize` | TTS: text → audio bytes |
| POST | `/api/v1/completions` | Direct LLM (stateless, no session) |
| GET | `/api/media/{id}` | Serve rendered media (JPEG/PNG/WAV), Cache-Control 1h |
| POST | `/api/media/upload` | Accept BMP/JPEG from Tab5 camera, return media_id |

## System

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v1/system` | System metrics (CPU, RAM, connections) |
| GET | `/api/v1/backends` | List available STT/TTS/LLM backends |

## Tools

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v1/tools` | List available tools |
| POST | `/api/v1/tools/{name}/execute` | Execute a tool directly |

## Memory

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/v1/memory` | List stored facts |
| POST | `/api/v1/memory` | Store a fact |
| DELETE | `/api/v1/memory/{id}` | Delete a fact |
| POST | `/api/v1/memory/search` | Semantic search facts |

## Documents

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/api/v1/documents` | Ingest document (chunk + embed) |
| GET | `/api/v1/documents` | List documents |
| DELETE | `/api/v1/documents/{id}` | Delete document + chunks |
| POST | `/api/v1/documents/search` | Semantic search across chunks |

## Notes

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/api/notes` | Create note |
| GET | `/api/notes` | List notes |
| GET | `/api/notes/{id}` | Get note |
| PUT | `/api/notes/{id}` | Update note |
| DELETE | `/api/notes/{id}` | Delete note |
| POST | `/api/notes/search` | Semantic search notes |
| POST | `/api/notes/from-audio` | Create note from audio |

## OTA (Tab5 firmware)

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/ota/check` | Check firmware updates (`?current=VERSION`) |
| GET | `/api/ota/firmware.bin` | Download firmware binary |

## Scheduler

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/api/v1/scheduler/notifications` | Schedule a notification |
| GET | `/api/v1/scheduler/notifications` | List notifications |
| GET | `/api/v1/scheduler/notifications/{id}` | Get one notification |
| DELETE | `/api/v1/scheduler/notifications/{id}` | Cancel a pending notification |
| PATCH | `/api/v1/scheduler/notifications/{id}` | Reschedule (change `when`) |

## Video relay (debug)

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/api/video/inject` | Push a JPEG into the relay as if from a Tab5 (debug) |

## Discovery + health

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/health` | Liveness check (no auth) |
| GET | `/version` | Server version (no auth) |

## Pagination

Endpoints returning lists support `?limit=N&offset=N` plus a `next_offset` field in the response for cursor-style iteration.

## Error format

All error responses are JSON:

```json
{
  "error": "machine_readable_code",
  "message": "human readable description",
  "details": { "..." }
}
```

HTTP status codes follow standard semantics (400 for validation errors, 401 for auth, 404 for not-found, 500 for server bugs).
