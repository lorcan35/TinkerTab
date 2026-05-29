---
audience: developer
type: reference
prerequisites: none
last-verified: 2026-05-29
---
# Firmware file map

Authoritative, lookup-oriented map of the [Tab5](../../GLOSSARY.md) firmware
source under [`main/`](../../main/). No tutorials here — use this to find the
right home for a change. If you add or remove a file, update this page and the
`CLAUDE.md` "Key Files" section.

## Boot + infrastructure

| File | Owns |
|---|---|
| `main/main.c` | Boot sequence: HW init → service bring-up → [LVGL](../../GLOSSARY.md) → watchdog; one-time init for audio cues + the notification snooze walker. |
| `main/config.h` | Pin map, firmware version, OTA paths, voice-mode constants. |
| `main/service_registry.{c,h}` | Service-pattern scaffolding (lifecycle for the five services). |
| `main/service_audio.c` | Audio service (I2S, ES8388 DAC, ES7210 mic). |
| `main/service_display.c` | LVGL + DSI panel bring-up. |
| `main/service_dragon.c` | [Dragon](../../GLOSSARY.md)-link service (voice WS lifecycle). |
| `main/service_network.c` | Wi-Fi service (STA, connect, reconnect). |
| `main/service_storage.c` | SD card + NVS storage service. |
| `main/task_worker.{c,h}` | Shared FreeRTOS job queue (kills per-action task leaks). |
| `main/heap_watchdog.{c,h}` | Periodic heap + PSRAM monitoring → `/heap`. |

## Debug server

The debug server was thinned (TT #332, Wave 23b) from a 4,520-LOC monolith to
849 LOC + 18 per-family modules. The core file owns the httpd lifecycle, bearer
auth, and the family register calls.

| File | Owns |
|---|---|
| `main/debug_server.{c,h}` | httpd lifecycle, bearer auth, `send_json_resp`, `/info` / `/index` / `/selftest`, 18 register calls. |
| `main/debug_server_admin.c` | `/reboot`, `/sdcard`, widget injection. |
| `main/debug_server_call.c` | `/video/*`, `/call/*`. |
| `main/debug_server_camera.c` | `/screenshot`, `/screenshot.jpg`, `/camera`. |
| `main/debug_server_chat.c` | `/chat`, `/chat/*`. |
| `main/debug_server_codec.c` | `/codec/opus_test`. |
| `main/debug_server_dictation.c` | `/dictation` (GET + POST). |
| `main/debug_server_inject.c` | `/debug/inject_audio`, `_error`, `_ws` (harness only). |
| `main/debug_server_input.c` | `/touch`, `/input/text` (LVGL touch-injection seqlock). |
| `main/debug_server_m5.c` | `/m5`, `/m5/reset`, `/m5/refresh`, `/m5/models`. |
| `main/debug_server_metrics.c` | `/tasks`, `/logs/tail`, `/battery`, `/display/brightness`, `/audio`, `/metrics`, `/events`, `/heap/history`, `/net/ping`. |
| `main/debug_server_mode.c` | `/mode`. |
| `main/debug_server_nav.c` | `/navigate`, `/screen`. |
| `main/debug_server_obs.c` | `/log`, `/crashlog`, `/coredump`, `/heap_trace_*`, `/heap`. |
| `main/debug_server_ota.c` | `/ota/check`, `/ota/apply`. |
| `main/debug_server_settings.c` | `/settings` (GET + POST), `/nvs/erase`. |
| `main/debug_server_voice.c` | `/voice`, `/voice/reconnect`, `/voice/cancel`, `/voice/clear`. |
| `main/debug_server_wifi.c` | `/wifi/kick`, `/wifi/status`. |
| `main/debug_server_internal.h` | shared `check_auth` + `send_json_resp` helpers. |
| `main/debug_obs.{c,h}` | the 256-entry observability event ring behind `/events`. |

Count live endpoints: `grep -c 'httpd_register_uri_handler' main/debug_server*.c`.

## Hardware drivers

| File | Owns |
|---|---|
| `main/audio.c` | ES8388 DAC via esp_codec_dev + STD TX / TDM RX I2S. |
| `main/mic.c` | ES7210 quad-mic via esp_codec_dev. |
| `main/camera.{c,h}` | esp_video V4L2 stack, SC202CS sensor. |
| `main/imu.c` | BMI270 IMU via I2C. |
| `main/sdcard.{c,h}` | SDMMC 4-bit, FAT32. |
| `main/wifi.{c,h}` | Wi-Fi stack wrapper. |
| `main/settings.{c,h}` | NVS-backed settings (see [NVS settings reference](nvs-settings.md)). |

## Dragon voice link

| File | Owns |
|---|---|
| `main/voice.{c,h}` | Voice public API + state machine + mic-capture/playback tasks + reconnect watchdog + channel-reply arming. |
| `main/voice_ws_proto.{c,h}` | WS frame routing: JSON RX dispatcher, binary magic-tag dispatcher, `esp_websocket_client` callback, UI-async helpers. |
| `main/voice_modes.{c,h}` | The six-tier [voice-mode](voice-modes.md) dispatcher + `voice_modes_route_text`. |
| `main/voice_video.{c,h}` | Two-way video call: HW JPEG uplink + TJPGD downlink, `VID0` framing. |
| `main/voice_codec.{c,h}` | OPUS capability negotiation (decoder ready, encoder gated off — issue #264). |
| `main/mode_manager.{c,h}` | Voice-pipeline coordinator (IDLE ↔ VOICE); thin mutex wrapper. |

## K144 / TinkerON

| File | Owns |
|---|---|
| `bsp/tab5/uart_port_c.{c,h}` | Port C UART (recursive mutex serializes every K144 transaction). |
| `main/m5_stackflow.{c,h}` | [StackFlow](../../GLOSSARY.md) JSON marshalling. |
| `main/voice_m5_llm.{c,h}` | K144 sidecar (infer / TTS / wakeword setup-run-teardown). |
| `main/voice_onboard.{c,h}` | vmode=4 lifecycle: warmup, per-text failover, autonomous chain, `reset_failover`. |
| `main/voice_wakeword.{c,h}` | always-on wakeword matcher + state machine. |

## UI framework + screens

| File | Owns |
|---|---|
| `main/ui_core.{c,h}` | LVGL display init, screen management, `tab5_lv_async_call`. |
| `main/ui_theme.{c,h}` | theme tokens. |
| `main/ui_orb.{c,h}` | the ambient orb (5 hardware-aware behaviors, 4-state machine). |
| `main/ui_home.{c,h}` | home (orb, clock, greeting, mode chip, nav sheet, widget slots). |
| `main/ui_voice.{c,h}` | voice overlay. |
| `main/ui_chat.{c,h}` + `chat_*.{c,h}` | chat overlay + header / input bar / message store / view / drawer / suggestions. |
| `main/ui_settings.{c,h}` | settings overlay. |
| `main/ui_wifi.{c,h}` / `ui_keyboard.{c,h}` | Wi-Fi setup / on-screen keyboard. |
| `main/ui_camera.{c,h}` / `ui_video_pane.{c,h}` | camera viewfinder / downlink video pane. |
| `main/ui_files.{c,h}` / `ui_notes.{c,h}` / `ui_sessions.{c,h}` / `ui_memory.{c,h}` | file browser / notes / sessions / memory. |
| `main/ui_agents.{c,h}` | agents / TinkerClaw status + agent-skills chips. |
| `main/ui_audio_cues.{c,h}` | pre-computed PCM cue buffers. |
| `main/ui_notification.{c,h}` | channel_message router + dedupe/snooze rings + reply bridge. |
| `main/ui_nav_sheet.{c,h}` / `ui_mode_sheet.{c,h}` / `ui_onboarding.{c,h}` | nav sheet / mode picker / first-boot flow. |
| `main/voice_dictation.{c,h}` | dictation state machine + LVGL subscriber wrapper. |

## Widgets + media + OTA

| File | Owns |
|---|---|
| `main/widget.h` / `main/widget_store.c` | widget data model + bounded priority queue. |
| `main/media_cache.{c,h}` | HTTP image downloader + PSRAM LRU + TJPGD decode. |
| `main/ota.{c,h}` | OTA check/apply/mark-valid via `esp_https_ota` with auto-rollback. |
| `partitions.csv` | dual-slot OTA partition table. |
| `LEARNINGS.md` | institutional knowledge (read before any change). |

## See also

- [Hardware reference](hardware.md) · [Debug server reference](debug-server.md)
- [TinkerTab architecture](../explanation/architecture.md) — the layering these
  files implement.
- `CLAUDE.md` "Key Files" — the canonical, always-current list.
