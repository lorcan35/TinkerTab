# Tab5 Firmware Glossary

> Terms specific to TinkerTab (the firmware repo).  For shared terms
> across both repos (router, fleet, modality, capability, etc.) see
> [TinkerBox's GLOSSARY.md](https://github.com/lorcan35/TinkerBox/blob/main/GLOSSARY.md).

## A

**`auth_tok`** — NVS key (32-char hex string) holding the bearer token for the Tab5 firmware's debug HTTP server on port 8080.  Auto-generated on first boot via `esp_random()`; persists across reboots.  Different from `DRAGON_API_TOKEN` (which gates Dragon's REST endpoints).

**`aut_tier`** — NVS key (uint8 0..1) — autonomy dial.  `0=ask first`, `1=agent mode` (engages TinkerClaw Gateway via `voice_mode=3`).  Lives in the v4·D Sovereign-Halo three-dial config.

## B

**BSP** — Board Support Package.  [`bsp/tab5/`](bsp/tab5/) holds the M5Stack Tab5 hardware abstraction — pin definitions, codec init, display setup, IO expander helpers.  See [`docs/HARDWARE.md`](docs/HARDWARE.md) for the full pinout.

## C

**`cam_rot`** — NVS key (uint8 0..3) — camera frame rotation in 90° steps.  `0=none, 1=90°CW, 2=180°, 3=270°CW`.  Applied in software after V4L2 capture before display, photo save, video record, and Dragon upload.  See PR #261/#290.

**`cap_mils`** — NVS key (uint32) — daily LLM-spend cap in mils (1/1000 ¢).  Default 100000 (= $1.00/day).  When `spent_mils` exceeds this on a Cloud-mode turn, Tab5 auto-downgrades to Local mode and announces `cap_downgrade` via `config_update`.

**`chat.llm_done`** — Debug-obs event kind emitted by `voice.c` when the WS `llm_done` message is received.  Detail field is the latency_ms.  Used by the e2e harness to know an LLM turn finished.

**Connection mode** (`conn_m`) — NVS key (uint8 0..2) — `0=auto` (try ngrok first then LAN), `1=local-only`, `2=remote-only`.  Tab5-internal — never sent to Dragon; affects only the Tab5 client's WS connect strategy.

**Counter-init** — Camera screen's `IMG_NNNN.jpg` index recovery.  On first open, `opendir("/sdcard")` + readdir scan finds the highest existing IMG_NNNN.jpg and resumes the counter from there.  Avoids overwriting on re-flash.  See [`ui_camera.c`](main/ui_camera.c).

## D

**Debounce** (navigation) — `/navigate` HTTP handler enforces a 600 ms minimum between consecutive nav requests so rapid taps + animation race conditions don't crash the LVGL overlay system.  See [`debug_server.c`](main/debug_server.c).

**`device_id`** — NVS key (12-hex chars).  Stable identifier sent in WS `register` frame.  MAC-derived on first boot.  Dragon uses this as the primary key in the `devices` table.

**`dragon_host` / `dragon_port`** — NVS keys.  Dragon WS server endpoint.  Defaults from `config.h` / Kconfig but settings UI lets the user change them.  PR [#299](https://github.com/lorcan35/TinkerTab/issues/299) was triggered when the e2e harness's `/input/text` accidentally typed into this field; PR [#300](https://github.com/lorcan35/TinkerTab/pull/300) scoped `/input/text` to the chat input only.

## E

**E2E harness** — Python scenario runner in [`tests/e2e/`](tests/e2e/) (PR #295).  `Tab5Driver` class wraps the debug HTTP API; `runner.py` runs scenarios with per-step screenshots + event captures + report.json/report.md output.  Three canonical stories: `story_smoke` (~2 min, 14 steps), `story_full` (~2 min, 24 steps), `story_stress` (~10 min, 77 steps).

**`/events` ring** — Debug server endpoint backed by a 256-slot ring buffer of `tab5_debug_obs_event(kind, detail)` calls.  Polled-only — see LEARNINGS for why long-poll was tried + reverted (PANIC under load on single-threaded httpd).  Events surfaced: `obs.init`, `screen.navigate`, `voice.state`, `ws.connect/disconnect`, `chat.llm_done`, `camera.capture`, `camera.record_start/stop`, `display.brightness`, `audio.volume`, `audio.mic_mute`, `nvs.erase`.

## F

**FATFS LFN** — `CONFIG_FATFS_LFN_NONE=1` in sdkconfig.defaults — long-filename support is OFF to save RAM.  Forces 8.3 short names.  Only affects code that creates files: video recording uses `.MJP` instead of `.mjpeg` because of this.  See LEARNINGS.

**FreeRTOS task workers** — Long-running concerns each get a persistent FreeRTOS task (mic capture, video stream, voice WS, etc.).  Per PR #285, the per-call-spawned task pattern (which leaked) was replaced with persistent-task-with-binary-semaphore-trigger pattern.  16 KB stacks live on PSRAM via `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`.

## H

**Heap watchdog** — Periodic monitor in `heap_watchdog.c` that checks internal SRAM largest free block every 30 s.  If the largest block stays below 30 KB for 3 minutes sustained, triggers a controlled reboot.  Hide/show pattern for overlays is the primary fragmentation defense; the watchdog is the safety net.

**Hide/show overlays** — Pattern from PR #183: Settings, Chat, and Voice overlays are created ONCE and hidden via `lv_obj_add_flag(LV_OBJ_FLAG_HIDDEN)`, never destroyed-and-recreated.  Internal SRAM (~512 KB) fragments quickly under create/destroy churn; hide/show keeps the allocations stable.

## I

**IO Expander** — Two PI4IOE5V6416 chips on system I2C (0x43 + 0x44) controlling power rails (LCD reset, speaker enable, WiFi power, USB 5 V, charging, etc.).  See [`docs/HARDWARE.md`](docs/HARDWARE.md).

**`int_tier`** — NVS key (uint8 0..2) — intelligence dial.  `0=fast`, `1=balanced`, `2=smart`.  Combined with `voi_tier` and `aut_tier` in `tab5_mode_resolve()` to derive the effective `voice_mode` (0/1/2/3).

## L

**LVGL** — Light and Versatile Graphics Library v9.2.2 (managed component).  Native UI framework — every screen / widget / overlay is LVGL.  Critical config in `sdkconfig.defaults`:
- `LV_MEM_SIZE_KILOBYTES=96` (BSS base pool, soft ceiling on this firmware layout)
- `LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096` (TLSF per-pool max-size — NOT auto-expand-on-demand)
- `LV_DRAW_SW_CIRCLE_CACHE_SIZE=32`
- `LV_DISPLAY_RENDER_MODE_PARTIAL` with two 144 KB draw buffers

`main.c` calls `lv_mem_add_pool()` at boot with a 2 MB PSRAM chunk — that's the actual heap LVGL operates on.

**`lv_async_call`** — LVGL primitive for scheduling work on the LVGL thread.  **NEVER call directly** — use [`tab5_lv_async_call`](main/ui_core.h) instead.  The LVGL primitive is NOT thread-safe (does `lv_malloc` + `lv_timer_create` against unprotected TLSF — caused the long-residual stability class closed in PR #257/#259).

## M

**`media_id`** — 32-char hex string (SHA-256 of resized image bytes).  Returned by Dragon's `POST /api/media/upload`; sent back in WS `user_image` event.  Persists in Dragon's `messages` table inside the `__mm__:` content marker.

**`mic_mute`** — NVS key (uint8 0..1) — master mic mute.  When set, `voice_start_listening` refuses with a toast.  Settings toggle; visible on the home mode chip.

**MJPEG / `.MJP`** — File format for video recording.  Concatenated JPEGs, no container, no audio.  Forced to `.MJP` (3-char extension) because FATFS LFN is disabled.  ffmpeg + VLC play these natively: `ffplay -f mjpeg VID_NNNN.MJP`.

## N

**Nav sheet** — Bottom-up swipe-up sheet that surfaces home / chat / notes / settings / camera / files / call.  Implemented in [`ui_nav_sheet.c`](main/ui_nav_sheet.c).

**NVS** — Non-Volatile Storage.  ESP-IDF flash partition holding key-value settings.  See [`CLAUDE.md`](CLAUDE.md) "NVS Settings Keys" for the canonical table.  Namespace is `"settings"`; max key length 15 chars.

## O

**`onboard`** — NVS key (uint8 0..1).  `1` once the user clears the welcome screens on first boot.  Reset by NVS erase.

**OPUS** — Audio codec for the WS streaming path.  Capability negotiation works (PR #263/#265).  Decoder ready on Tab5; **encoder gated OFF** in [`voice_codec.h`](main/voice_codec.h) pending issue [#264](https://github.com/lorcan35/TinkerTab/issues/264) — SILK NSQ crashes mid-frame on ESP32-P4.  Don't enable uplink until #264 closes.

## P

**Partial render mode** — LVGL's incremental rendering strategy.  Renders into a small buffer that gets flushed to the DPI panel; the alternative (DIRECT mode) tears on this hardware.  Two 144 KB draw buffers in PSRAM.

**Persistent task pattern** — PR #285 design: long-running tasks (mic capture, video streaming) are created ONCE at boot.  They idle on a binary semaphore between sessions.  Replaces the older per-call `xTaskCreate + vTaskSuspend` pattern that leaked tasks under load.

**PIP** — Picture-in-picture.  240×135 local-camera preview shown in the corner of the video-call pane while in-call.

**Power rails** — Switched via the two IO expanders.  LCD, speaker, WiFi (ESP32-C6), USB host, charging are all software-controllable.

## R

**REC button** — Red-bordered pill in the camera viewfinder control bar (right of the shutter circle).  Toggles motion-JPEG recording to `/sdcard/VID_NNNN.MJP` at 5 fps.  PR #291.  Live overlay shows `REC ▶ MM:SS  N KB` at the top during recording.

**Rich Media** — `media`, `card`, `audio_clip`, `text_update` WS messages from Dragon.  Code blocks render as syntax-highlighted JPEGs (Pygments), tables as styled grids (Pillow), images downloaded + cached.  5-slot PSRAM LRU cache (~2.9 MB).

**`Rot` button** — In-viewfinder button on the camera screen.  Cycles `cam_rot` 0→1→2→3→0 in one tap and recreates the screen so the rotation applies live.  Saves a trip to Settings.  PR #290.

## S

**`screen.navigate`** — Debug-obs event kind emitted by `debug_server.c` navigate handler.  Detail = target screen name.

**Session** — Multi-turn conversation owned by Dragon (not Tab5).  Tab5 stores the `session_id` in NVS for resume.  Tab5 doesn't manage its own session lifecycle — Dragon does that.

**`spent_mils` / `spent_day`** — NVS keys tracking today's cumulative LLM spend in mils.  `spent_day` = days-since-epoch of the last `spent_mils` write.  Resets when the day rolls.  u32 with daily writes — wear bounded to ~hundreds of commits/day.

**Soft-reset stress** — Boot path optimization in PR #285+: persistent tasks survive `esp_restart()` no-ops cleanly because they're created at boot and idle on semaphores between bursts.  No reboot loop is needed for routine state changes.

## T

**Tab5** — M5Stack Tab5 hardware: ESP32-P4 + 5" 720×1280 IPS + MIPI-CSI camera + 4-mic + DAC + WiFi via hosted ESP32-C6 + 16 MB flash + 32 MB PSRAM + 6 Ah LiPo + capacitive touch.  See [`docs/HARDWARE.md`](docs/HARDWARE.md).

**`tab5_lv_async_call(cb, arg)`** — Wrapper in [`main/ui_core.{c,h}`](main/ui_core.h) around `lv_async_call`.  Takes the LVGL recursive mutex first.  **Always use this** instead of the LVGL primitive directly — the primitive is not thread-safe.  PR #257/#259 wrapped all 49 sites; rule going forward.

**`task_worker`** — Shared FreeRTOS job queue ([`task_worker.{c,h}`](main/task_worker.c)).  Long-running uploads / downloads / etc. enqueue here instead of spawning per-action tasks.

**Tile-view** — LVGL tileview widget — main home screen layout pre-v4·C.  Currently retired in favor of single Ambient Canvas page.  References to "page 0", "page 1" etc. in older docs are tile-view era.

**Toast** — Transient bottom-of-screen notification used for low-priority feedback (ack, error, status).  Auto-dismisses after 2-3 s.

**Touch (debug)** — `POST /touch` endpoint accepts `{x, y, action: "tap|press|release|long_press|swipe", duration_ms?, x1?, y1?, x2?, y2?}`.  Tap = 200 ms hold (LVGL CLICKED).  Long_press = 500-5000 ms (LVGL LONG_PRESSED at 400 ms).  Swipe = 20 ms step cadence between (x1,y1) and (x2,y2) over duration_ms.

## U

**UI surface** — LVGL screen abstraction.  Tab5 has 7 screens (splash/home/chat/notes/settings/camera/files) + 2 overlays (keyboard/voice).  Most overlays use the hide/show pattern; chat-screen-from-overlay-tap uses destroy-recreate (`New Chat` button).

**`user_image` / `user_media`** — WS messages Tab5 sends to Dragon announcing a previously-uploaded image.  Triggers Dragon's vision-turn flow.  See [TinkerBox `flows/vision-turn.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/flows/vision-turn.md).

## V

**v4·C Ambient Canvas** — Current home-screen visual language.  Single page (no tile-view), clock + orb + greeting + mode pill + nav sheet + widget slots.  Replaced earlier multi-page design.

**`VID0`** — 4-byte ASCII magic on a binary WS frame indicating it's a JPEG video frame for the call relay.  Counterpart: `AUD0` for raw 16 kHz mono int16 PCM.

**`vmode`** — NVS key (uint8 0..3) — voice mode.  `0=Local`, `1=Hybrid`, `2=Cloud`, `3=TinkerClaw`.  Sent to Dragon as `voice_mode` in `config_update`.

**`voi_tier`** — NVS key (uint8 0..2) — voice dial.  `0=local Piper`, `1=neutral`, `2=studio OpenRouter`.  Combined with `int_tier` + `aut_tier` to derive the effective voice mode.

**`voice.state`** — Debug-obs event kind emitted by `voice_set_state()` on every state transition.  Detail = state name (IDLE/CONNECTING/READY/LISTENING/PROCESSING/SPEAKING/RECONNECTING).  e2e harness's `await_voice_state()` watches for these.

**`VOICE_MODE_CALL`** — Internal voice-pipeline state independent of `voice_mode` 0/1/2/3.  When active: mic frames are wrapped with `AUD0` magic prefix and broadcast to other call participants instead of being fed to STT.  Set by `voice_video_start_call()` and reset by `voice_video_end_call()`.

**`voice_video.{c,h}`** — Two-way video calling module.  HW JPEG uplink via the shared encoder + TJPGD downlink decode + `VID0` framing.  `voice_video_start_call` / `voice_video_end_call` are the atomic entry points.  Also exposes `voice_video_encode_rgb565()` so the camera-screen recording feature shares the single HW JPEG engine.

## W

**Watchdog reset** — Triggered when (a) the heap watchdog declares fragmentation crisis, or (b) `python -m esptool ... --after watchdog_reset` from a workstation.  Tab5's USB-JTAG doesn't wire RTS to EN, so `default_reset` / `hard_reset` won't cut it for a running app — must use `watchdog_reset`.

**Widget** — Skill-emitted state rendered by Tab5.  Six types: `live`, `card`, `list`, `chart`, `media`, `prompt`.  Spec in [`docs/WIDGETS.md`](docs/WIDGETS.md); implementation plan in [`docs/PLAN-widget-platform.md`](docs/PLAN-widget-platform.md).  See also TinkerBox's `surfaces/`.

**WS** — WebSocket.  Tab5 has exactly one persistent WS connection to Dragon at `ws://<host>:3502/ws/voice`.  All voice/text/vision/video/config/event traffic multiplexes over this single channel.

## Cross-references

- **Operating runbook** (deploy, debug, restart, monitor): [`CLAUDE.md`](CLAUDE.md)
- **Hardware pinout + IC list**: [`docs/HARDWARE.md`](docs/HARDWARE.md)
- **Voice pipeline spec**: [`docs/VOICE_PIPELINE.md`](docs/VOICE_PIPELINE.md)
- **Widget spec**: [`docs/WIDGETS.md`](docs/WIDGETS.md)
- **E2E harness**: [`tests/e2e/README.md`](tests/e2e/README.md)
- **Lessons + gotchas**: [`LEARNINGS.md`](LEARNINGS.md)
- **System architecture (Dragon side)**: [TinkerBox `docs/ARCHITECTURE.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/ARCHITECTURE.md)
- **Wire format**: [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md)
- **Cross-repo glossary** (router, fleet, modality, etc.): [TinkerBox `GLOSSARY.md`](https://github.com/lorcan35/TinkerBox/blob/main/GLOSSARY.md)
