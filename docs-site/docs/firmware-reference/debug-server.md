---
title: Debug server
sidebar_label: Debug server
---

# Debug server

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

A bearer-auth-gated HTTP server inside the firmware on port **8080**. Enables ADB-style remote control: take screenshots, drive touch, switch voice modes, capture camera frames, query diagnostics.

## Authentication

All endpoints except `/info` and `/selftest` require a Bearer token in the `Authorization` header.

- **Token generation** — first boot generates a 32-char hex token via `esp_random()` and saves to NVS key `auth_tok`. Persists across reboots.
- **Display** — printed to serial log on every boot: `I (xxx) debug_srv: Debug server auth token: <token>`
- **Public endpoints** — `GET /info` (device discovery, includes `"auth_required":true`) and `GET /selftest` (health check)

```bash
# Get the token from serial output, then:
export TOKEN="abcdef1234567890abcdef1234567890"
```

## Endpoint catalog

### Display + screenshot

```bash
# 720x1280 BMP screenshot of the live framebuffer
curl -s -H "Authorization: Bearer $TOKEN" \
     -o screen.bmp http://<tab5-ip>:8080/screenshot

# Discovery (no auth needed)
curl -s http://<tab5-ip>:8080/info | python3 -m json.tool

# Adjust display brightness 0-100
curl -X POST -H "Authorization: Bearer $TOKEN" \
     "http://<tab5-ip>:8080/display/brightness?p=50"
```

### Touch + input

```bash
# Tap at (x, y)
curl -X POST -H "Authorization: Bearer $TOKEN" \
     -d '{"x":360,"y":640,"action":"tap"}' \
     http://<tab5-ip>:8080/touch

# Long-press 1.2s
curl -X POST -H "Authorization: Bearer $TOKEN" \
     -d '{"x":360,"y":640,"action":"long_press","duration_ms":1200}' \
     http://<tab5-ip>:8080/touch

# Swipe 600,640 → 120,640 over 300ms
curl -X POST -H "Authorization: Bearer $TOKEN" \
     -d '{"action":"swipe","x1":600,"y1":640,"x2":120,"y2":640,"duration_ms":300}' \
     http://<tab5-ip>:8080/touch

# Type into focused LVGL textarea
curl -X POST -H "Authorization: Bearer $TOKEN" \
     -d '{"text":"hello world","submit":true}' \
     http://<tab5-ip>:8080/input/text
```

### Voice + chat

```bash
# Switch voice mode (0=Local, 1=Hybrid, 2=Cloud, 3=TinkerClaw, 4=Onboard)
curl -X POST -H "Authorization: Bearer $TOKEN" \
     "http://<tab5-ip>:8080/mode?m=2&model=anthropic/claude-sonnet-4-20250514"

# Send text to Dragon (skips STT)
curl -X POST -H "Authorization: Bearer $TOKEN" \
     -d '{"text":"What time is it?"}' \
     http://<tab5-ip>:8080/chat

# Voice state snapshot
curl -H "Authorization: Bearer $TOKEN" \
     http://<tab5-ip>:8080/voice | python3 -m json.tool

# Force voice WS reconnect
curl -X POST -H "Authorization: Bearer $TOKEN" \
     http://<tab5-ip>:8080/voice/reconnect
```

### Navigation

```bash
# Force screen change
curl -X POST -H "Authorization: Bearer $TOKEN" \
     "http://<tab5-ip>:8080/navigate?screen=settings"
# Valid screens: home, chat, voice, settings, camera, notes, files,
#                memory, sessions, agents
```

### Camera + media

```bash
# Capture live frame as BMP
curl -H "Authorization: Bearer $TOKEN" \
     -o frame.bmp http://<tab5-ip>:8080/camera

# Video call control
curl -X POST -H "Authorization: Bearer $TOKEN" http://<tab5-ip>:8080/call/start
curl -X POST -H "Authorization: Bearer $TOKEN" http://<tab5-ip>:8080/call/end
curl -H "Authorization: Bearer $TOKEN" http://<tab5-ip>:8080/video | jq
```

### K144 diagnostics

```bash
# Snapshot
curl -H "Authorization: Bearer $TOKEN" http://<tab5-ip>:8080/m5 | jq
# → {"chain_active":false, "failover_state":2, "uart_baud":115200,
#    "hwinfo":{"temp_celsius":39.35, "cpu_loadavg":0, "mem":27},
#    "version":"v1.3"}

# Software reset (~9.6s end-to-end)
curl -X POST -H "Authorization: Bearer $TOKEN" \
     http://<tab5-ip>:8080/m5/reset

# Force-refresh hwinfo cache
curl -X POST -H "Authorization: Bearer $TOKEN" \
     http://<tab5-ip>:8080/m5/refresh

# Model registry
curl -H "Authorization: Bearer $TOKEN" \
     "http://<tab5-ip>:8080/m5/models?force=1" | jq
```

### Settings + observability

```bash
# Read all NVS settings as JSON
curl -H "Authorization: Bearer $TOKEN" \
     http://<tab5-ip>:8080/settings | jq

# Observability event ring (since uptime_ms=N)
curl -H "Authorization: Bearer $TOKEN" \
     "http://<tab5-ip>:8080/events?since=0" | jq

# Heap stats
curl -H "Authorization: Bearer $TOKEN" http://<tab5-ip>:8080/heap | jq

# Audio
curl -X POST -H "Authorization: Bearer $TOKEN" \
     "http://<tab5-ip>:8080/audio?volume=80"
curl -X POST -H "Authorization: Bearer $TOKEN" \
     "http://<tab5-ip>:8080/audio?mic_mute=1"
```

## Used by the e2e harness

The Python end-to-end harness in [`tests/e2e/`](https://github.com/lorcan35/TinkerTab/tree/main/tests/e2e) drives Tab5 through long user-story flows entirely via this debug server. See `tests/e2e/driver.py` for the `Tab5Driver` wrapper and `tests/e2e/scenarios/` for the scenario library.
