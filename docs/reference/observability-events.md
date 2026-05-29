---
audience: integrator
type: reference
prerequisites: none
last-verified: 2026-05-29
---
# Observability events reference

Authoritative, lookup-oriented reference for the [Tab5](../../GLOSSARY.md)'s
observability event ring. No tutorials here — to drive the device with these
events see [Run the e2e test harness](../how-to/run-the-e2e-test-harness.md).

`tab5_debug_obs_event(kind, detail)` (in
[`main/debug_obs.c`](../../main/debug_obs.c)) writes to a **256-entry FIFO ring**
(oldest evicted). `GET /events?since=N` on the [debug server](debug-server.md)
returns every entry with `ms >= N`. Each entry is
`{"ms": uptime_ms, "kind": "category.subkind", "detail": "..."}`. The `kind`
buffer is **32 chars**, `detail` is **48 chars** — both silently truncate past
those limits.

> **Polling only.** A `/events` long-poll was tried and reverted — ESP-IDF's
> httpd is single-task, so `vTaskDelay` blocked all other requests and
> fragmented the heap (PANIC under load). Poll at ~250 ms instead.

## Event kinds

| Kind | Where it fires | Detail |
|---|---|---|
| `obs` | Boot | `"init"` once. |
| `screen.navigate` | `POST /navigate` handler | target screen name. |
| `voice.state` | `voice_set_state()` on every transition | `IDLE` / `CONNECTING` / `READY` / `LISTENING` / `PROCESSING` / `SPEAKING` / `RECONNECTING`. |
| `ws.connect` / `ws.disconnect` | WS event handler | empty. |
| `chat.llm_done` | WS `llm_done` handler | `llm_ms` value as a string. |
| `camera.capture` | `capture_btn_cb` after photo saved | absolute SD path. |
| `camera.record_start` / `camera.record_stop` | record toggle | path; `path frames=N bytes=N` on stop. |
| `display.brightness` | `POST /display/brightness` | percentage. |
| `audio.volume` / `audio.mic_mute` | `POST /audio` | new value. |
| `nvs` | `POST /nvs/erase` | `"erase"`. |
| `m5.warmup` | `voice_onboard_warmup_job` + `reset_failover_job` | `start` / `ready` / `unavailable`. |
| `m5.chain` | chain start/stop | `start` / `stop`. |
| `m5.reset` | `voice_onboard_reset_failover` | `start` / `ack_ok` / `ack_fail` / `auto_retry` / `recovered` / `fail`. |
| `error.k144` | `mark_k144_unavailable` | `probe_fail` / `warmup_fail` / `reset_probe_fail` / `reset_warmup_fail`. |
| `ui.cue` | `ui_audio_cues.c::cue_play_job` | `{cue_name} err={n}` (mode_switch / cancel / error / incoming_low / incoming_high). |
| `ui.notif` | `ui_notification.c` after route decision | `{ch}/{sender} pri={p} surf={now\|toast}[ quiet]`. |
| `ui.notif.channel_off` | router gate when `ch_*_on=0` | `{channel}`. |
| `ui.notif.dedupe` | dedupe-ring hit | `drop {message_id}`. |
| `ui.notif.now` | now-card button taps | `dismiss` / `reply` / `snooze`. |
| `ui.notif.snooze` | snooze ring + walker | `added {ch}/{sender} in=900s` / `overflow` / `defer_quiet` / `refire`. |
| `ui.notif.reply` | `channel_reply` round-trip | `armed ...` / `dictated ...` / `sent ...` / `ack_ok` / `ack_fail {err}` / `no_cache` / `send_fail err={n}`. |
| `agent_skills` | `ui_agents.c::fetch_agent_skills_job` | `count={n} observed={k}` or `skip vmode={n}`. |

## Reading the ring

```bash
export TOKEN="abcdef1234567890abcdef1234567890"

# Everything since boot
curl -s -H "Authorization: Bearer $TOKEN" "http://<ip>:8080/events?since=0" \
     | python3 -m json.tool

# Only events after uptime_ms=50000 (a tighter window for one test step)
curl -s -H "Authorization: Bearer $TOKEN" "http://<ip>:8080/events?since=50000" \
     | python3 -m json.tool
```

A sample entry:

```json
{"ms": 52310, "kind": "voice.state", "detail": "LISTENING"}
```

## Notes

- **Truncation is a known footgun.** `camera.record_start` once truncated to
  `camera.record_s` against the old 16-char `kind` buffer (bumped to 32). Keep
  new `kind` strings under 32 chars and `detail` under 48.
- **Cursor management.** The e2e harness must not let a diagnostic read advance
  its event cursor — use `events(peek=True)` for snapshots, or
  `await_voice_state` (which also polls `/voice` current state) to dodge the
  boot-`READY` race.

## See also

- [Debug server reference](debug-server.md) — the `/events` endpoint + the rest
  of the control API.
- [Run the e2e test harness](../how-to/run-the-e2e-test-harness.md) — the
  harness that polls these events.
- [TinkerTab architecture](../explanation/architecture.md) — why this
  observability surface is part of the design.
