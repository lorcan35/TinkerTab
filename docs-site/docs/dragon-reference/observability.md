---
title: Observability events
sidebar_label: Observability events
---

# Observability events

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Dragon emits typed events to a ring buffer that's queryable via `GET /api/v1/events`. Same vocabulary as Tab5's `/events` ring (debug server) — readable by the dashboard, the e2e harness, and any external monitoring.

## Why a ring buffer

- **Bounded memory** — 1024 events on Dragon, 256 on Tab5. Old events drop off; new ones append.
- **Polling, not push** — clients fetch with `?since=N` (uptime ms). Tried long-polling once on Tab5; the single-task ESP-IDF httpd panicked under load. Polling at 250 ms is the safe choice.
- **Cross-component vocabulary** — `screen.navigate`, `voice.state`, `tool_call`, `error.*` mean the same thing whether they came from Tab5 or Dragon

## Event shape

```json
{
  "ms": 12345678,            // uptime ms
  "kind": "tool_call",       // category.subkind, max 32 chars
  "detail": "web_search",    // free-form, max 48 chars (Tab5) / 256 chars (Dragon)
  "session_id": "...",       // optional
  "device_id": "..."         // optional
}
```

## Categories

### Lifecycle

| Kind | Detail |
|------|--------|
| `obs` | `init` (boot) / `shutdown` |
| `boot` | `started` |
| `monitor.rss` | `MB=1234` |
| `monitor.fd` | `count=N` |
| `monitor.temp` | `celsius=42.3` |

### Voice

| Kind | Detail |
|------|--------|
| `voice.state` | `IDLE` / `CONNECTING` / `READY` / `LISTENING` / `PROCESSING` / `SPEAKING` / `RECONNECTING` |
| `ws.connect` | (empty) |
| `ws.disconnect` | reason / `unknown` |
| `chat.llm_done` | `llm_ms` value |

### Tools + agent

| Kind | Detail |
|------|--------|
| `tool_call` | tool name |
| `tool_result` | `tool=NAME ms=N` |
| `tool_error` | `tool=NAME error=...` |

### Camera + display

| Kind | Detail |
|------|--------|
| `camera.capture` | absolute SD path |
| `camera.record_start` / `camera.record_stop` | path + frame/byte counts on stop |
| `display.brightness` | percentage |
| `audio.volume` / `audio.mic_mute` | new value |

### Screen

| Kind | Detail |
|------|--------|
| `screen.navigate` | target screen name |

### NVS

| Kind | Detail |
|------|--------|
| `nvs` | `erase` / `set:<key>` |

### K144 (Tab5-side, surfaced via debug events)

| Kind | Detail |
|------|--------|
| `m5.warmup` | `start` / `ready` / `unavailable` |
| `m5.chain` | `start` / `stop` |
| `m5.reset` | `start` / `ack_ok` / `ack_fail` / `auto_retry` / `recovered` / `fail` |
| `error.k144` | reason: `probe_fail` / `warmup_fail` / `reset_probe_fail` / `reset_warmup_fail` |

## Polling clients

```bash
# Tab5
curl -H "Authorization: Bearer $TOKEN" \
     "http://<tab5-ip>:8080/events?since=0" | jq

# Dragon
curl -H "Authorization: Bearer $TOKEN" \
     "http://<dragon>:3502/api/v1/events?since=0&limit=200" | jq
```

`since=0` returns the entire ring; pass the highest `ms` from the previous response to fetch only new events.

## Filtering

```bash
# Only tool_call events
curl -H "Authorization: Bearer $TOKEN" \
     "http://<dragon>:3502/api/v1/events?type=tool_call" | jq

# By session
curl -H "Authorization: Bearer $TOKEN" \
     "http://<dragon>:3502/api/v1/events?session_id=..." | jq

# By device
curl -H "Authorization: Bearer $TOKEN" \
     "http://<dragon>:3502/api/v1/events?device_id=..." | jq
```

## Used by the e2e harness

`tab5.await_event(kind, timeout_s, detail_match=...)` polls `/events` for a matching event with a 250 ms cadence + cursor-based dedup. See `tests/e2e/driver.py` for the wrapper.

## Used by the dashboard

The Logs tab is a live event stream — the dashboard polls every 1 s and renders new events. Filter chips for kind, session, device.

## When to add a new event kind

If you're investigating a bug + reaching for `printf`, that's a sign you should emit an obs event instead. The event kind goes into the table here. Tab5: edit `main/debug_obs.{c,h}`; Dragon: edit `dragon_voice/api/events.py`.

`kind` buffer is 32 chars; `detail` is 48 chars on Tab5 (silently truncated past) and 256 chars on Dragon.
