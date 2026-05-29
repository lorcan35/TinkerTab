---
audience: integrator
type: reference
prerequisites: none
last-verified: 2026-05-29
---
# Debug server reference

Authoritative, lookup-oriented reference for the Tab5's HTTP debug server. No
tutorials here — for a guided first run see the
[getting-started tutorial](../tutorials/getting-started.md).

When Wi-Fi is connected, the [Tab5](../../GLOSSARY.md) runs an HTTP debug server
on **port 8080** for remote testing and control (screenshots, touch injection,
navigation, voice control, K144/[TinkerON](../../GLOSSARY.md) diagnostics, and
more). The Tab5's IP is DHCP-assigned; locate it with
`nmap -p 8080 --open <subnet>/24` — the Tab5 is the host whose `GET /info`
returns `"auth_required": true`.

## Authentication

All endpoints **except `/info` and `/selftest`** require a bearer token in the
`Authorization` header.

| Item | Detail |
|---|---|
| Token generation | A 32-char random hex token is generated via `esp_random()` on first boot. |
| Token storage | [NVS](../../GLOSSARY.md) namespace `"settings"`, key `"auth_tok"`, max 32 chars. Persists across reboots. |
| Token display | Printed to the serial log on every boot: `I (xxx) debug_srv: Debug server auth token: <token>`. |
| Public endpoints | `GET /info` (device discovery, includes `"auth_required":true`) and `GET /selftest` (health check) require no auth. |
| Usage | Add `-H "Authorization: Bearer <token>"` to every request except `/info` and `/selftest`. |

```bash
# Capture the token from serial, then export it for reuse:
export TOKEN="abcdef1234567890abcdef1234567890"
```

## Core endpoints

| Method | Path | Auth | Description |
|---|---|---|---|
| GET | `/info` | no | Device info JSON (heap, peripherals, mode, `auth_required`). |
| GET | `/selftest` | no | Health check JSON. |
| GET | `/screenshot` | yes | Framebuffer as BMP (720×1280 RGB565). |
| GET | `/screenshot.jpg` | yes | Framebuffer as JPEG. |
| GET | `/camera` | yes | Live camera frame (SC202CS) as JPEG/BMP. |
| POST | `/touch` | yes | Inject a touch event (see below). |
| POST | `/input/text` | yes | Type into the focused LVGL textarea. |
| POST | `/navigate?screen=<name>` | yes | Force a screen change. |
| GET | `/screen` | yes | Current screen + overlay visibility. |
| POST | `/reboot` | yes | Restart the device. |
| GET | `/events?since=<ms>` | yes | Observability event ring (poll-only). |

## Touch injection

`POST /touch` accepts a JSON body. Actions: `tap`, `long_press`, `swipe`.

```bash
# Tap at (360, 640)
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/touch \
     -d '{"x":360,"y":640,"action":"tap"}'

# Long press (450 ms default; 500-5000 ms via duration_ms)
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/touch \
     -d '{"x":360,"y":640,"action":"long_press","duration_ms":1200}'

# Swipe (50-3000 ms via duration_ms)
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/touch \
     -d '{"action":"swipe","x1":600,"y1":640,"x2":120,"y2":640,"duration_ms":300}'
```

## Voice + mode endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/voice` | Voice state (connected, state_name, last_llm_text, last_stt_text). |
| POST | `/voice/reconnect` | Force a voice WebSocket reconnect. |
| POST | `/chat` | Send text to the Dragon over the voice WS (`{"text":"..."}`). |
| POST | `/mode?m=<0-5>[&model=<id>]` | Switch [voice mode](../../GLOSSARY.md) (and optional LLM model). |
| GET | `/dictation_pipeline` | Dictation state machine snapshot. |

```bash
# Switch to Local (0), Hybrid (1), Cloud (2 + model), TinkerClaw (3)
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://<ip>:8080/mode?m=0"
curl -s -H "Authorization: Bearer $TOKEN" -X POST \
     "http://<ip>:8080/mode?m=2&model=anthropic/claude-sonnet-4-20250514"

# Send a text turn
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/chat \
     -d '{"text":"What time is it?"}'
```

## K144 / TinkerON diagnostics

| Method | Path | Description |
|---|---|---|
| GET | `/m5` | Chain snapshot: `chain_active`, `failover_state`, `uart_baud`, cached `hwinfo`. |
| POST | `/m5/reset` | Send `sys.reset` to the [StackFlow](../../GLOSSARY.md) daemon and re-run warmup. |
| POST | `/m5/refresh` | Force a fresh `sys.hwinfo` + `sys.version` round-trip. |
| GET | `/m5/models[?force=1]` | Installed-model registry (`sys.lsmode`). |

```bash
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/m5 | python3 -m json.tool
# → {"chain_active":false,"failover_state":2,"failover_state_name":"ready",
#    "uart_baud":115200, ...}
```

## OTA + storage

| Method | Path | Description |
|---|---|---|
| GET | `/ota/check` | Check for an available firmware update. |
| POST | `/ota/apply` | Apply the available update (downloads, verifies SHA256, reboots). |
| GET | `/settings` | Read all NVS settings as JSON. |
| POST | `/settings` | Write NVS settings. |
| POST | `/nvs/erase` | Erase NVS. |

## The handler module map (post-Wave-23b)

The debug server was refactored (TT #332, Wave 23b) from a single 4,520-LOC
`debug_server.c` down to 849 LOC plus 18 per-family handler modules. The core
file owns the httpd lifecycle, bearer-token auth (`init_auth_token`,
`tab5_debug_check_auth`), the shared `send_json_resp` helper, the public
`/info` / `/index` / `/selftest`, and the 18 family register calls. Each family
lives in its own file:

| Module | Endpoints it owns |
|---|---|
| `debug_server_admin.c` | `/reboot`, `/sdcard`, widget injection |
| `debug_server_call.c` | `/video/*`, `/call/*` (video-call endpoints) |
| `debug_server_camera.c` | `/screenshot`, `/screenshot.jpg`, `/camera` |
| `debug_server_chat.c` | `/chat`, `/chat/messages`, `/chat/llm_done`, `/chat/partial`, `/chat/audio_clip` |
| `debug_server_codec.c` | `/codec/opus_test` |
| `debug_server_dictation.c` | `/dictation` (GET + POST) |
| `debug_server_inject.c` | `/debug/inject_audio`, `_error`, `_ws` (harness only) |
| `debug_server_input.c` | `/touch`, `/input/text` (LVGL touch-injection seqlock) |
| `debug_server_m5.c` | `/m5`, `/m5/reset`, `/m5/refresh`, `/m5/models` |
| `debug_server_metrics.c` | `/tasks`, `/logs/tail`, `/battery`, `/display/brightness`, `/audio`, `/metrics`, `/events`, `/heap/history`, `/net/ping` |
| `debug_server_mode.c` | `/mode` (voice mode + LLM model picker) |
| `debug_server_nav.c` | `/navigate`, `/screen` |
| `debug_server_obs.c` | `/log`, `/crashlog`, `/coredump`, `/heap_trace_*`, `/heap` |
| `debug_server_ota.c` | `/ota/check`, `/ota/apply` |
| `debug_server_settings.c` | `/settings` (GET + POST), `/nvs/erase` |
| `debug_server_voice.c` | `/voice`, `/voice/reconnect`, `/voice/cancel`, `/voice/clear` |
| `debug_server_wifi.c` | `/wifi/kick`, `/wifi/status` |
| `debug_server_internal.h` | shared `check_auth` + `send_json_resp` helpers |

Count the live endpoints at any time with:

```bash
grep -c 'httpd_register_uri_handler' main/debug_server*.c
```

## Examples

A full discovery → screenshot round-trip:

```bash
# 1. Find the Tab5 (no auth) and confirm it's the right device
curl -s http://<ip>:8080/info | python3 -m json.tool   # auth_required: true

# 2. Grab the token from the serial boot log, then capture the screen
export TOKEN="abcdef1234567890abcdef1234567890"
curl -s -H "Authorization: Bearer $TOKEN" -o screen.bmp http://<ip>:8080/screenshot
```
