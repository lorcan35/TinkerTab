---
title: Logs & monitoring
sidebar_label: Logs & monitoring
---

# Logs & monitoring

Where to look when something's off. Three layers, increasing depth.

## Layer 1 — systemd journal

```bash
# Live tail
sudo journalctl -u tinkerclaw-voice -f

# Last 50 lines
sudo journalctl -u tinkerclaw-voice --no-pager -n 50

# Errors only since boot
sudo journalctl -u tinkerclaw-voice -p err --no-pager
```

Look for:

- **`session_start`** events — every Tab5 connect logs one with the device_id
- **`tool_call` / `tool_result`** — every tool execution
- **`error.*`** — anything that went wrong (auth, backend, scheduler)
- **`router: chose <model> for <caps>`** — every router decision (in fleet mode)

## Layer 2 — the dashboard

Open `http://<dragon>:3500/` (or the ngrok-exposed `tinkerclaw-dashboard.ngrok.dev`).

Eleven tabs:

1. **Overview** — system status, active connections, backend config, CPU + RAM bars
2. **Conversations** — browse all sessions, view message history, filter by device
3. **Chat** — live SSE-streaming chat (stateless mode for quick queries)
4. **Devices** — registered Tab5s, online/offline status, capabilities
5. **Notes** — notes CRUD + search + audio-to-note
6. **Logs** — event-log explorer with type/session/device filters
7. **Memory** — stored facts with semantic search bars
8. **Documents** — ingested documents + chunk browser + search
9. **Tools** — list of registered tools + direct execute UI
10. **OTA** — firmware update management
11. **Debug** — 55-test E2E suite + Tab5 remote-control panel

The dashboard proxies all API calls to the voice server (port 3502), so you can restart the dashboard without dropping voice sessions.

## Layer 3 — the REST API

```bash
# Active sessions
curl -H "Authorization: Bearer $TOKEN" \
     http://localhost:3502/api/v1/sessions?status=active

# Recent events (filter by type or session)
curl -H "Authorization: Bearer $TOKEN" \
     "http://localhost:3502/api/v1/events?type=tool_call&limit=50"

# Cross-session agent activity
curl -H "Authorization: Bearer $TOKEN" \
     http://localhost:3502/api/v1/agent_log

# System metrics
curl -H "Authorization: Bearer $TOKEN" \
     http://localhost:3502/api/v1/system | jq
# → {"cpu_pct":12.3, "ram_pct":54.1, "uptime_s":12345, "active_sessions":2, ...}
```

The agent_log feed (TT #328 Wave 12) is particularly useful — it's a 64-slot ring buffer populated at the `ToolRegistry.execute` chokepoint, so every tool call (from a WS conversation, a REST execute, or the dashboard) shows up in one place.

## Specific failure-mode signposts

| Symptom | First thing to check |
|---------|----------------------|
| Tab5 shows "Voice WS not connected" | `journalctl -u tinkerclaw-voice` for crash; `curl http://localhost:3502/health` |
| LLM seems to hang for >5 min | `ollama ps` — is the model evicted? bump RAM or change model |
| Tool calls "succeed" but reply is empty | check the response_wrap logs — small FC models often need this |
| Memory recall feels weak | lower `memory.recall_threshold` in `config.yaml` |
| Cloud mode 401s | verify `OPENROUTER_API_KEY` in `~/.env`; restart |
| Dragon high RAM | check `ollama ps` for resident models; consider eviction policy |
| Dashboard shows "Disconnected" | `systemctl status tinkerclaw-dashboard` — port 3500 separately from voice |

## Heap watchdog

Dragon's `lifecycle/monitors.py` runs a 5-minute periodic check on RSS, CPU temp, and FD count. If RSS exceeds 4 GB or FD count exceeds 1024, it logs a `monitor.*` event and (in extreme cases) drains the pipelines. Tune in `config.yaml` if your hardware has different headroom.

## Backups

The whole Dragon state lives in **two places**:

```bash
~/.tinkerclaw/dragon.db           # sessions, messages, memory, scheduler, devices
~/.tinkerclaw/media/              # photo + audio uploads, 24-hour TTL
```

`cp ~/.tinkerclaw/dragon.db ~/backup-$(date +%Y%m%d).db` is the entire backup story. Restore by stopping `tinkerclaw-voice`, replacing the file, restarting.
