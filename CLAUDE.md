# TinkerTab — TinkerOS Firmware (ESP32-P4 Tab5) — THE FACE

## Active Investigations — READ FIRST before related work

- **LVGL pool pressure (stability)** → [`docs/STABILITY-INVESTIGATION.md`](docs/STABILITY-INVESTIGATION.md)
  Ongoing multi-phase investigation into the ~170 s crash cadence
  observed under the stress orchestrator. **Do NOT start one-off crash
  patches** if a new LVGL-internal crash surfaces — read the state doc
  + the phase plan at
  [`docs/superpowers/plans/2026-04-23-lvgl-pool-investigation.md`](docs/superpowers/plans/2026-04-23-lvgl-pool-investigation.md)
  first. Use the `superpowers:executing-plans` skill to resume where
  the previous session left off. The plan is root-cause driven; the
  whack-a-mole era ended with PR #178.

## Repo Separation — READ THIS FIRST
- **TinkerTab** (this repo) = Tab5 firmware. C/ESP-IDF. THIN CLIENT.
  - Owns: LVGL UI, mic/speaker/camera/touch, SD card, WiFi, NVS settings
  - Sends: audio frames, text messages, device registration over WebSocket
  - Receives: STT results, LLM responses, TTS audio, config updates
  - Storage: SD card for local audio recordings + offline queue. NVS for settings. NO database.
- **TinkerBox** (github.com/lorcan35/TinkerBox) = Dragon Q6A server. Python. ALL intelligence.
  - Owns: STT, LLM, TTS, embeddings, sessions, conversation engine, REST API, dashboard, database
  - Tab5 is a thin client. Dragon is the brain.
- **Protocol:** See TinkerBox `docs/protocol.md` for the WebSocket contract. Tab5 implements the client side.

## Overview
TinkerTab is the TinkerOS firmware for the M5Stack Tab5, part of the TinkerClaw platform.
- **Target audience:** Privacy-conscious tech enthusiasts (r/selfhosted, r/localllama). Setup under 5 minutes. Not tinkerers-only.

Companion repo: [TinkerBox](https://github.com/lorcan35/TinkerBox) (Dragon-side server)

## TinkerClaw Vision (March 2026)

### Architecture
- Tab5 = the face (LVGL native UI, voice, camera, sensors)
- Dragon Q6A = the brain (STT, LLM on NPU at 8 tok/s, TTS, embeddings, skills, memory)
- Any ESP32/Pico/edge device = a TinkerClaw Node
- PingOS app manifests = how AI interacts with web services (browser automation via CDP)
- Local-first, cloud-optional. NPU Llama 1B for offline, cloud APIs for smart mode.

### Priority Order
1. VAD + AEC + Wake Word ("Hey Tinker") — makes it hands-free, THE product feature
2. Desktop SDL2 simulator + dev tooling (fast dev loop without flashing)
3. Spring animations + battery UI + OTA (polish)
4. On-device skills/app store + offline knowledge + multi-modal vision

### Key Decisions
- **Voice-first, not a remote display.** The CDP browser-streaming path that
  Tab5 shipped with (MJPEG + touch relay over port 3501) was retired in
  #155 — the product is the voice assistant.
- **Double down on voice** — that's what differentiates from every IoT display.
- Memory/RAG via Qwen3-Embedding 0.6B on Dragon. AI that remembers you.
- Skill store is the growth flywheel. Tinkerers publish, normies install.

### What to Steal from M5Stack
- Desktop SDL2 simulator build pattern (from M5Tab5-UserDemo)
- sherpa-onnx for KWS + silero-vad for VAD (both CPU ONNX, hardware-agnostic)
- Toast notification pattern, spring animation concept (write our own spring_anim.c)

### What NOT to Do
- Don't adopt Mooncake (our service_registry is better for embedded)
- Don't rewrite to C++
- Don't build a full browser on Tab5
- Don't reintroduce CDP browser streaming — it's gone; voice-first is the product

### Our Unique Advantage
- Dragon Q6A: 12GB RAM, 12 TOPS NPU vs M5Stack AX630C (4GB, 3.2 TOPS)
- Full Ubuntu on Dragon = can run anything (NOMAD, Kiwix, databases)
- Truly open — standard tools (Ollama, QAIRT/Genie, Python), not vendor-locked
- LEARNINGS.md with 30+ entries = institutional knowledge no competitor has

## Workflow
1. **Issue first** — Create a GitHub issue before starting work (`gh issue create`)
2. **Branch** — Create a feature/fix branch from main
3. **Commit with issue ref** — Every commit must reference an issue (`refs #N` or `closes #N`)
4. **Push and merge** — Push to origin, merge to main

### Issue Format
```
## Bug / Problem
What the user sees. Symptoms, frequency, impact.

## Root Cause
Technical explanation of WHY this happens.

## Culprit
Exact file(s) and line(s) responsible.

## Fix
What was changed and why this fixes it.

## Resolved
Commit hash + PR if applicable.
```

### Commit Messages
- Reference issues: `fix: description (closes #N)` or `feat: description (refs #N)`
- Keep commits atomic — one fix per commit, push after each
- Never batch multiple unrelated fixes into one commit

## Dragon Access (for deploying/testing)
- **Host:** 192.168.1.91
- **User:** radxa (NOT rock — user was migrated)
- **Password:** `<DRAGON_SSH_PASSWORD>`
- **SSH:** `ssh radxa@192.168.1.91  # password in ~/.ssh/config or use key auth`
```bash
ssh radxa@192.168.1.91  # password in ~/.ssh/config or use key auth
sudo systemctl status tinkerclaw-voice
sudo journalctl -u tinkerclaw-voice --no-pager -n 50
```

### Dragon Stability
- **Ethernet** — DHCP IP 192.168.1.91 on enp1s0
- **Stripped services** — gdm3, snapd, ollama, nanobot, rustdesk, fwupd all masked. Only tinkerclaw-* services run.
- **ngrok tunnels** — `tinkerclaw-ngrok.service` maintains three tunnels:
  - `tinkerclaw-dashboard.ngrok.dev` → localhost:3500 (dashboard)
  - `tinkerclaw-voice.ngrok.dev` → localhost:3502 (voice WS)
  - `tinkerclaw-gateway.ngrok.dev` → localhost:18789 (TinkerClaw gateway)
- **Tab5 ngrok fallback** — voice.c tries local WS first, falls back to wss://tinkerclaw-voice.ngrok.dev:443

## Build & Flash
```bash
# Always use ESP-IDF v5.5.2 — matches dependencies.lock
. /home/rebelforce/esp/esp-idf/export.sh

cd /home/rebelforce/projects/TinkerTab

# Clean build (after sdkconfig changes, pull new components, or target changes):
idf.py set-target esp32p4
idf.py build

# Flash to device:
idf.py -p /dev/ttyACM0 flash

# If the board enters ROM download mode after flashing (common on ESP32-P4),
# trigger a watchdog reset to boot the flashed app:
python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac
```

### Monitor Serial Output
```bash
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=5)
time.sleep(0.3)
s.write(b'\r')
while True:
    if s.in_waiting:
        print(s.read(s.in_waiting).decode('utf-8', errors='replace'), end='', flush=True)
"
```

## Debug Server (ADB-style Remote Control)
Full HTTP API on port 8080 for remote testing and control.
**Note:** Tab5 IP is DHCP-assigned. Current lease: 192.168.1.90. Update if it changes.

### Authentication (Bearer Token)
All endpoints except `/info` and `/selftest` require a Bearer token in the `Authorization` header.
- **Token generation:** On first boot, a 32-char random hex token is generated via `esp_random()` and saved to NVS key `"auth_tok"`.
- **Token display:** Printed to serial log on every boot: `I (xxx) debug_srv: Debug server auth token: <token>`
- **Token storage:** NVS namespace `"settings"`, key `"auth_tok"`, max 32 chars. Persists across reboots.
- **Public endpoints (NO auth):** `GET /info` (device discovery, includes `"auth_required":true`), `GET /selftest` (health check)
- **Usage:** Add `-H "Authorization: Bearer <token>"` to all curl commands (except /info and /selftest)

```bash
# Get the token from serial output, then export it:
export TOKEN="abcdef1234567890abcdef1234567890"

# Display
curl -s -H "Authorization: Bearer $TOKEN" -o screen.bmp http://192.168.1.90:8080/screenshot
curl -s http://192.168.1.90:8080/info | python3 -m json.tool   # No auth needed

# Touch
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/touch -d '{"x":360,"y":640,"action":"tap"}'

# Settings (read all NVS settings as JSON)
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.1.90:8080/settings | python3 -m json.tool

# Voice Mode (switch Local/Hybrid/Cloud remotely)
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/mode?m=0"  # Local
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/mode?m=1"  # Hybrid (cloud STT+TTS)
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/mode?m=2&model=anthropic/claude-sonnet-4-20250514"  # Full Cloud
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/mode?m=3"  # TinkerClaw

# Navigation (force screen change — bypasses tileview)
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=settings"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=notes"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=chat"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=camera"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.1.90:8080/navigate?screen=home"

# Camera (capture live frame as BMP)
curl -s -H "Authorization: Bearer $TOKEN" -o frame.bmp http://192.168.1.90:8080/camera

# OTA
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.1.90:8080/ota/check | python3 -m json.tool
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/ota/apply

# Chat (send text to Dragon via voice WS)
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/chat -d '{"text":"What time is it?"}'

# Voice state (connected, state_name, last_llm_text, last_stt_text)
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.1.90:8080/voice | python3 -m json.tool

# Force voice WS reconnect
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.1.90:8080/voice/reconnect

# Self-test (no auth needed)
curl -s http://192.168.1.90:8080/selftest | python3 -m json.tool
```

## ESP32-P4 Memory Rules
- **PSRAM for large buffers (>4KB):** Use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`. Static BSS arrays eat limited internal SRAM (~512KB shared with FreeRTOS).
- **Always free PSRAM:** Use `heap_caps_free()`, not `free()`.
- **No vTaskDelete(NULL):** Use `vTaskSuspend(NULL)` — P4 TLSP cleanup crash (issue #20).
- **Stack sizes:** SDIO tasks need 8K+. Mic task 4K (TDM buffer in PSRAM). WS task 8K.
- **PSRAM cache coherency:** DPI DMA reads PSRAM directly. Call `esp_cache_msync()` after CPU writes to framebuffer.
- **Internal SRAM fragmentation:** `heap_caps_get_free_size()` lies — total free != usable. Check `heap_caps_get_largest_free_block()` for the largest contiguous block. Fragmentation from overlay create/destroy cycles makes total free look healthy while allocations fail. Use hide/show pattern for overlays to prevent fragmentation. Fragmentation watchdog reboots after 3min sustained low largest-block.

## Audio Pipeline Rules
- **I2S mixed mode:** TX = STD Philips (ES8388 DAC), RX = TDM 4-slot (ES7210 ADC), both on I2S_NUM_1. Matches M5Stack BSP exactly. ESP32-P4 supports mixed STD/TDM on same port.
- **ES8388 init:** NEVER write custom registers. Use `es8388_codec_new()` from esp_codec_dev library. Custom init had 5 register differences that prevented audio (issue #46).
- **Playback:** All audio via `esp_codec_dev_write()` through `tab5_audio_play_raw()`. Playback drain task (voice.c) handles producer-consumer I2S writes.
- **Slot mode:** Use `I2S_SLOT_MODE_STEREO` for TDM multi-slot capture. MONO only gets slot 0.
- **Sample rates:** Hardware runs at 48kHz. Downsample 3:1 to 16kHz for STT. Upsample 1:3 from 16kHz for TTS playback.
- **esp_codec_dev:** Uses 8-bit I2C addresses (ES7210=0x80, ES8388=0x20). NOT 7-bit.
- **Mic audio:** Slot 0 = MIC-L (primary). Extract from 4-ch TDM interleaved buffer.

## IDF Version
- **Use IDF v5.5.2** — matches `dependencies.lock`. Build always requires `idf.py set-target esp32p4` after any clean build or target change. Run `idf.py fullclean build` when in doubt.
- Tab5 camera is SC202CS at SCCB 0x36 (NOT SC2336 at 0x30)
- SD card uses SDMMC SLOT 0 with LDO channel 4

## Voice Pipeline
### Core Features
- **Ask Mode:** Short-form voice: PTT → mic stream → Dragon STT → LLM → TTS playback (30s max)
- **Dictation Mode:** Long-press mic → unlimited recording → STT-only → auto-stop 5s silence → Dragon post-processing generates title + summary
- **Text Input:** `voice_send_text()` sends `{"type":"text","content":"..."}` — skips STT, same conversation context
- **Boot Auto-Connect:** Voice WS connects at boot (silent — no overlay popup). Device registers immediately. Session created on boot. Eliminates 5-15s connect delay on first mic tap.
- **Reconnect Watchdog:** Checks connection every 5s. If Dragon restarts or network drops, auto-reconnects with exponential backoff (10s→20s→40s→60s max). Idle keepalive pings detect dead TCP.
- **Offline Fallback:** If Dragon is unreachable on mic tap, auto-starts local SD card recording via Notes module.

### Three-Tier Voice Mode
Settings dropdown: **Local / Hybrid / Full Cloud / TinkerClaw**

| Mode | STT | LLM | TTS | Latency | Cost |
|------|-----|-----|-----|---------|------|
| Local (0) | Moonshine | NPU/Ollama (default: qwen3:1.7b, 7.1 tok/s) | Piper | 2-5s | Free |
| Hybrid (1) | OpenRouter gpt-audio-mini | Local (unchanged) | OpenRouter gpt-audio-mini | 4-8s | ~$0.02/req |
| Full Cloud (2) | OpenRouter gpt-audio-mini | User-selected (Haiku/Sonnet/GPT-4o) | OpenRouter gpt-audio-mini | 3-6s | $0.03-0.08/req |
| TinkerClaw (3) | Moonshine (or OpenRouter) | TinkerClaw Gateway | Piper (or OpenRouter) | varies | varies |

- **LLM Model Picker:** Settings dropdown: Local NPU / Local Ollama / Claude Haiku / Claude Sonnet / GPT-4o mini (enabled only in Full Cloud mode)
- **Auto-Fallback:** If cloud STT/TTS fails, Dragon falls back to local for that request + sends `config_update` with `error` field → Tab5 auto-reverts to Local mode
- **NVS:** `voice_mode` (uint8: 0/1/2/3), `llm_model` (string: model ID)
- **Protocol:** `{"type":"config_update","voice_mode":0|1|2|3,"llm_model":"anthropic/claude-sonnet-4-20250514"}`

## OTA Firmware Updates
- **Dual OTA partitions:** ota_0 (3MB) + ota_1 (3MB) with otadata boot selector
- **Check:** `tab5_ota_check()` → GET `http://dragon:3502/api/ota/check?current=0.8.0`
- **Apply:** `tab5_ota_apply(url, sha256)` → downloads via `esp_https_ota()`, verifies SHA256, writes inactive slot, reboots
- **SHA256 verification (SEC07):** After download, firmware is read back from the OTA partition and hashed with `mbedtls_sha256`. The computed hash is compared against the `sha256` field from `version.json`. Mismatch aborts the OTA with `ESP_ERR_INVALID_CRC`. If `sha256` is empty/NULL, a warning is logged but OTA proceeds (backward compat). This prevents MitM firmware swaps over unencrypted LAN HTTP.
- **Auto-rollback:** New firmware boots in PENDING_VERIFY. If crash before `tab5_ota_mark_valid()`, bootloader reverts.
- **Settings UI:** "Check Update" button shows version + partition. "Apply Update" green button appears when update available.
- **Debug:** `/ota/check` and `/ota/apply` endpoints on debug server
- **Deploy new firmware:**
  ```bash
  scp build/tinkertab.bin radxa@192.168.1.91:/home/radxa/ota/
  # IMPORTANT: Always include sha256 hash for MitM protection
  SHA=$(sha256sum build/tinkertab.bin | cut -d' ' -f1)
  echo "{\"version\":\"0.7.1\",\"sha256\":\"$SHA\"}" | ssh radxa@192.168.1.91 'cat > /home/radxa/ota/version.json'
  # Tab5 checks hourly or user taps "Check Update" in Settings
  ```

## Camera
- **Sensor:** SC202CS 2MP via MIPI-CSI (1-lane, 576MHz, RAW8)
- **Pipeline:** SC202CS → MIPI CSI → ISP (RAW8→RGB565) → /dev/video0 (V4L2)
- **Driver:** Uses `esp_video` + `esp_cam_sensor` stack from M5Stack (NOT raw CSI API)
- **Resolution:** 1280×720 @ 30fps RGB565
- **Exposure:** Tuned for indoor lighting (SCCB register writes post-init)
- **Debug:** `GET /camera` returns live frame as BMP
- **CONFIG_CAMERA_SC202CS=y** must be set in sdkconfig (sensor won't compile without it!)

### Dragon Server Intelligence
- **Mode-aware system prompts:** Local=concise (128 tokens), Hybrid=medium (256 tokens), Cloud=rich (512 tokens). Prompt matches model capability.
- **SearXNG web search:** Self-hosted on Dragon port 8888, aggregates Google+Bing+DDG (44 results/query). Falls back to DuckDuckGo if SearXNG unavailable.
- **Compact tool format:** `format_for_llm(compact=True)` for local models — 5 priority tools, ~150 tokens vs ~500 for full format.

### Remaining Gaps
- **AEC + Wake Word:** Removed in #162 after the TDM-slot-mapping blocker
  proved intractable without a custom WakeNet model.  The AFE/ESP-SR
  scaffolding is gone; the 3 MB `model` SPIFFS partition at 0x660000 is
  orphan but retained until a future PR repacks the partition table.
  Revival path: restore from git history + procure a custom "Hey Tinker"
  model from Espressif + re-do the TDM slot audit.
- **OPUS encoding:** 16kHz PCM = 256kbps. OPUS would cut to ~16kbps. Future optimization.

## Key Technical Notes
- ESP-IDF WS transport masks frames in-place — NEVER pass string literals to `esp_transport_ws_send_raw()`, always copy to mutable buffer first
- LVGL objects must not be accessed from background tasks after screen destroy — use `volatile bool s_destroying` guards
- All FreeRTOS tasks doing network + LVGL callbacks need minimum 8KB stack on ESP32-P4
- Screenshot handler must copy framebuffer under LVGL lock, then stream without lock
- `lv_screen_load_anim()` with auto_delete=false when returning to existing home screen
- sdkconfig changes always require `idf.py fullclean build` — incremental builds cache stale config

## NVS Settings Keys

All keys live in the `"settings"` NVS namespace. Max key length is 15 chars.

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `wifi_ssid` | str | `TAB5_WIFI_SSID` (config.h) | — | WiFi network SSID |
| `wifi_pass` | str | `TAB5_WIFI_PASS` (config.h) | — | WiFi network password |
| `dragon_host` | str | `TAB5_DRAGON_HOST` (config.h) | — | Dragon server hostname/IP |
| `dragon_port` | u16 | `TAB5_DRAGON_PORT` (config.h) | 1-65535 | Dragon server port |
| `brightness` | u8 | 80 | 0-100 | Display brightness percentage |
| `volume` | u8 | 70 | 0-100 | Speaker volume percentage |
| `device_id` | str | MAC-derived (12 hex chars) | — | Unique device identifier, auto-generated on first boot |
| `session_id` | str | `""` (empty) | — | Dragon conversation session ID for resume |
| `vmode` | u8 | 0 | 0-3 | Voice mode: 0=local, 1=hybrid, 2=cloud, 3=TinkerClaw |
| `llm_mdl` | str | `anthropic/claude-3.5-haiku` | — | LLM model identifier for cloud mode |
| `conn_m` | u8 | 0 | 0-2 | Connection mode: 0=auto (ngrok first then LAN), 1=local only, 2=remote only (Tab5-internal; never crosses the wire — see `voice_ws_start_client`) |
| `auth_tok` | str | auto-generated (32 hex chars) | — | Debug server bearer auth token, generated on first boot |
| `mic_mute` | u8 | 0 | 0-1 | Master mic mute — voice_start_listening refuses with a toast if set |
| `quiet_on` | u8 | 0 | 0-1 | Quiet hours master switch |
| `quiet_start` | u8 | 22 | 0-23 | Quiet-hours start hour (local clock, 24h) |
| `quiet_end` | u8 | 7 | 0-23 | Quiet-hours end hour; can wrap past midnight |
| `int_tier` | u8 | 0 | 0-2 | v4·D Sovereign-Halo intelligence dial (0=fast, 1=balanced, 2=smart) |
| `voi_tier` | u8 | 0 | 0-2 | v4·D voice dial (0=local Piper, 1=neutral, 2=studio OpenRouter) |
| `aut_tier` | u8 | 0 | 0-1 | v4·D autonomy dial (0=ask first, 1=agent mode) |
| `onboard` | u8 | 0 | 0-1 | Onboarding-flow completion marker (set once the user clears the welcome screens) |
| `spent_mils` | u32 | 0 | 0-UINT32_MAX | Today's cumulative LLM spend, in mils (1/1000 ¢). Resets when `spent_day` rolls to a new day. u32 with daily writes — wear bounded to ~one commit per LLM turn. |
| `spent_day` | u32 | 0 | 0-UINT32_MAX | Days-since-epoch of the last `spent_mils` write — the dayroll guard. |
| `cap_mils` | u32 | 100000 | 0-UINT32_MAX | Per-day spend cap in mils (default $1.00/day). Exceeding it triggers cap_downgrade in voice_send_config_update_ex. |

### ⚠️ LVGL Configuration — CRITICAL
**ALL LVGL config goes in `sdkconfig.defaults`, NOT `lv_conf.h`.** The ESP-IDF LVGL component sets `CONFIG_LV_CONF_SKIP=1` which means `lv_conf.h` is COMPLETELY IGNORED. Any change to `lv_conf.h` has ZERO effect. Always verify with `grep "SETTING" build/config/sdkconfig.h` after building.

Current LVGL settings in `sdkconfig.defaults`:
- **Memory pool:** `CONFIG_LV_MEM_SIZE_KILOBYTES=96` is the BSS-allocated base pool (soft ceiling for this firmware layout; 128+ aborts at boot in `main_task` per Phase 3-A). `CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096` is **only a TLSF per-pool max-size ceiling** (`TLSF_MAX_POOL_SIZE = LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE` at `lv_tlsf.c:13`) — **NOT** an auto-expand-on-demand trigger. LVGL 9.2.2 has no auto-expand; the only way to grow the heap is to call `lv_mem_add_pool()` ourselves. `main.c` does exactly that at boot with a 2 MB PSRAM-backed chunk, giving ~2048 + 96 KB of working heap. Verified under stress: `free_size` stabilises with generous headroom; zero LVGL PANIC crashes. See `docs/STABILITY-INVESTIGATION.md`.
- **Circle cache:** `CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE=32`. Default 4 causes crashes when 5+ rounded objects render simultaneously.
- **Asserts disabled:** `CONFIG_LV_USE_ASSERT_MALLOC=n` and `CONFIG_LV_USE_ASSERT_NULL=n` — prevents `while(1)` hang on alloc failure (which triggers 60s WDT reboot). With asserts off, NULL propagates and crashes faster with a useful backtrace instead of a silent WDT hang.
- **JPEG decode:** `CONFIG_LV_USE_TJPGD=y` + `CONFIG_LV_USE_FS_MEMFS=y` — enables TJPGD decoder for in-memory JPEG rendering (used by rich media chat).
- **Render mode:** `LV_DISPLAY_RENDER_MODE_PARTIAL` with two 144KB draw buffers in PSRAM. Do NOT use DIRECT mode (causes tearing on DPI).

### Key Fixes (April 2026)
- **LVGL pool OOM crash fixed:** Notes edit overlay exhausted the 96 KB base pool → `lv_malloc` NULL → NULL-deref inside LVGL draw pipeline → PANIC reboot (cadence ~170 s under stress). Full root-cause investigation in `docs/STABILITY-INVESTIGATION.md` across 12 whack-a-mole symptom PRs + Phase 1-3. Real fix: call `lv_mem_add_pool()` at boot with a 2 MB PSRAM chunk (`main.c`), because LVGL 9.2.2 has no auto-expand despite the misleading `CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES` name. 96 KB base is a soft ceiling at link time — don't bump it above 96.
- **Circle cache crash fixed:** 4 cache entries overflowed with 7+ rounded cards → `circ_calc_aa4` NULL dereference. Fix: `CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE=32` in `sdkconfig.defaults`.
- **Settings WDT crash fixed:** `f_getfree()` on a 128GB SD card blocks the LVGL thread for ~30s, triggering the watchdog. Fix: cache the `f_getfree` result from boot, feed `esp_task_wdt_reset()` between settings UI sections during creation.
- **Response timeout (mode-aware):** Local mode = 5 min timeout for tool-calling chains (small models are slow). Cloud mode = 1 min timeout. Timeout is mode-aware, set per voice mode.
- **Tool parser tolerant of small model quirks:** qwen3:1.7b adds stray `>` after `</args>`, sometimes omits closing tags. Parser uses tolerant regex with fallback patterns to handle these gracefully.
- **Speaker buzzing fixed:** IO expander P1 (SPK_EN) now initialized LOW at boot to prevent speaker buzzing on startup.
- **Settings rewrite:** Replaced with fullscreen overlay using manual Y positioning. No flex layout, no separate screen. Eliminates WDT crash + draw buffer exhaustion.
- **Settings two-pass creation:** Settings overlay uses a two-pass pattern — first pass creates the container, second pass populates sections with `esp_task_wdt_reset()` fed between each. Touch input blocked during creation to prevent partial-UI taps.
- **WiFi:** Switched to DHCP, WPA2-PSK router, lowered auth threshold.
- **Voice WS:** Added TLS cert bundle for ngrok, fixed TCP/SSL transport leaks.
- **Chat UI overhaul:** Live status bar (Ready/Processing/Speaking), tappable mode badge to cycle Local/Hybrid/Cloud, New Chat button, thinking + tool indicator bubbles during LLM processing.
- **Touch feedback system:** `ui_feedback.h/c` module with pressed states on 30+ interactive elements — buttons darken, cards lighten border, icons dim, nav items brighten. 100ms ease-out transitions.
- **Nav debounce 300ms:** Prevents rapid-tap crashes from animation race conditions (dismiss + create overlapping).
- **Nav bar on lv_layer_top:** Nav bar is rendered on `lv_layer_top()` so it remains accessible and visible regardless of which screen/overlay is active. Not inside the tileview.
- **TinkerClaw voice mode 3:** Added VOICE_MODE_TINKERCLAW=3 — routes LLM through TinkerClaw Gateway while STT/TTS use Moonshine/Piper locally or OpenRouter as fallback.
- **Voice overlay instant hide:** `ui_voice_hide()` is instant — no fade animation. The 150ms fade-out caused three bugs: (1) dangling `s_auto_hide` timer pointer (local static with `auto_delete=true` → use-after-free on next READY entry), (2) `fade_done_hide_cb` firing during navigation state changes, (3) `orb_speak_click_cb` stacking on SPEAKING re-entry. All three fixed April 2026.
- **Voice overlay shows on orb tap:** Tapping the orb on the home screen opens the voice overlay and starts listening immediately.
- **Bearer token auth on debug server:** All endpoints except `/info` and `/selftest` require a Bearer token. Token auto-generated on first boot via `esp_random()` and stored in NVS key `auth_tok`.
- **Keyboard text visible while typing:** Text input field stays visible above the keyboard during typing (was previously hidden behind the keyboard).
- **Done key auto-submits:** Done/Enter key on the keyboard dispatches `LV_EVENT_READY` instead of inserting a newline, triggering form submission.
- **Internal SRAM fragmentation monitoring:** Periodic heap check monitors largest free internal SRAM block (not just total free). If largest block stays below threshold for 3 minutes sustained, triggers a controlled reboot to defragment.
- **LVGL FPS counter:** LVGL flush rate, surfaced as `lvgl_fps` in `GET /info`, for monitoring UI rendering performance.
- **Rich Media Chat:** Chat screen renders inline rich media — syntax-highlighted code blocks, cards, and audio clips as JPEG images. Dragon renders content server-side (Pygments for code, Pillow for tables), Tab5 downloads and displays via LVGL's TJPGD decoder. 5-slot PSRAM LRU cache (~2.9MB) with zero alloc/free fragmentation. Max 3 media items per LLM response. Downloads yield every 2ms to not stall voice WS.

### Heap Fragmentation Fix (Hide/Show Pattern)
Internal SRAM on the ESP32-P4 (~512KB) fragments over time as overlays are created and destroyed. Total free memory may look healthy, but the largest contiguous block shrinks until allocations fail.

**Root cause:** Creating and destroying LVGL overlays (Settings, Chat, Voice) allocates and frees many small objects from internal SRAM. Over hours of use, this fragments the heap — `heap_caps_get_free_size()` reports adequate free memory, but `heap_caps_get_largest_free_block()` shows no single block large enough for new allocations.

**Fix — hide/show instead of create/destroy:**
- Settings, Chat, and Voice overlays are created ONCE and then hidden/shown via `lv_obj_add_flag(LV_OBJ_FLAG_HIDDEN)` / `lv_obj_remove_flag(LV_OBJ_FLAG_HIDDEN)`.
- `dismiss_all_overlays()` calls `ui_chat_hide()`, `ui_settings_hide()`, etc. — NOT destroy.
- Overlays with active timers or animations must be hidden, not destroyed, to prevent timer linked-list corruption.
- Destroy only when permanently replacing (e.g., New Chat button).

**Fragmentation watchdog:** `_memory_monitor` task checks internal SRAM largest free block every 30 seconds. If the largest block stays below 30KB for 3 minutes, triggers a controlled reboot. This is the safety net — the hide/show pattern is the primary fix.

### Global Typography System
All font usage across the UI is standardized through `config.h` defines:

| Define | Font | Size | Usage |
|--------|------|------|-------|
| `FONT_TITLE` | Montserrat Bold | 28px | Screen titles, section headers |
| `FONT_HEADING` | Montserrat Bold | 22px | Card titles, overlay headers |
| `FONT_BODY` | Montserrat | 18px | Body text, descriptions |
| `FONT_SMALL` | Montserrat | 14px | Timestamps, labels, metadata |
| `FONT_TINY` | Montserrat | 12px | Status bar, badges |
| `FONT_NAV` | Montserrat | 12px | Navigation bar labels |

All UI files use these defines instead of raw `lv_font_montserrat_XX` references. To change the global typography, update only `config.h`.

## WebSocket Protocol (Tab5 = Client Side)
See TinkerBox `docs/protocol.md` for the full spec. Tab5 responsibilities:

### Tab5 → Dragon (sending)
1. **Voice Input:** `{"type":"start"}` (with optional `"mode":"dictate"`) -> binary PCM frames -> `{"type":"stop"}`
2. **Voice Cancel:** `{"type":"cancel"}` — abort current processing
3. **Keepalive:** `{"type":"ping"}` — JSON heartbeat every 15s during processing
4. **Text Input:** `{"type":"text","content":"..."}` — skips STT, goes straight to LLM
5. **Config Update:** `{"type":"config_update","voice_mode":0|1|2|3,"llm_model":"..."}` — four-tier mode switch. Backward compat: `cloud_mode` bool still accepted.
6. **Device Registration:** `{"type":"register","device_id":"...","session_id":"..."}` on WS connect
7. **Clear History:** `{"type":"clear"}` — reset conversation context (W14-H20: was documented as `clear_history`; Tab5's `voice.c` + Dragon's dispatcher have always agreed on `clear`)

### Dragon → Tab5 (receiving)
1. **session_start** — session_id for NVS persistence
2. **stt** / **stt_partial** — transcription results (partial for dictation streaming)
3. **llm** — LLM response text (streamed)
4. **llm_done** — LLM generation complete with timing
5. **tool_call** — tool invocation event: `{"type":"tool_call","tool":"web_search","args":{"query":"..."}}` — display tool activity indicator
6. **tool_result** — tool completion event: `{"type":"tool_result","tool":"web_search","result":{...},"execution_ms":234}` — display tool result
7. **tts_start** / binary TTS / **tts_end** — TTS audio playback
8. **dictation_summary** — post-processing results: `{"title":"...","summary":"..."}`
9. **pong** — keepalive response
10. **config_update** — ACK with applied backend config + cloud_mode state
11. **error** — error details

### Dragon → Tab5 (Rich Media — April 2026)
12. **media** — inline rendered image: `{"type":"media","media_type":"image","url":"/api/media/abc.jpg","width":660,"height":400,"alt":"Code: python"}`. Tab5 creates placeholder bubble, spawns FreeRTOS task on Core 1, downloads JPEG via `media_cache_fetch()`, JPEG SOF parser extracts dimensions, `lv_async_call` swaps placeholder with `lv_image`.
13. **card** — rich preview card: `{"type":"card","title":"...","subtitle":"...","image_url":"...","description":"..."}`. Orange accent border.
14. **audio_clip** — inline audio: `{"type":"audio_clip","url":"...","duration_s":2.3,"label":"..."}`.
15. **text_update** — replace last AI bubble text: `{"type":"text_update","text":"cleaned text"}`. Dragon strips rendered code blocks from the text after sending them as media images.

### Not Yet Implemented (planned)
- **Recording:** `{"type":"record_start"}` -> binary PCM -> `{"type":"record_stop"}` — creates a note.
- **SD Card:** Save raw WAV to SD before/during WS send. Offline queue if Dragon unreachable. (Issue #44)

## Key Files

Generated from `ls main/` — if you add or remove a file, update this section.

### Boot + infrastructure
```
main/main.c               — Boot sequence: HW init → service bring-up → LVGL → watchdog
main/config.h             — Pin map, firmware version, OTA paths, VOICE_MODE_TINKERCLAW=3
main/service_registry.*   — Service-pattern scaffolding (audio/display/dragon/network/storage)
main/service_audio.c      — Audio service init (I2S, ES8388 DAC, ES7210 mic)
main/service_display.c    — LVGL + panel init, DSI display bring-up
main/service_dragon.c     — Dragon-link service (voice WS + mDNS + touch relay lifecycle)
main/service_network.c    — Wi-Fi service (STA, connect, reconnect)
main/service_storage.c    — SD card + NVS storage service
main/task_worker.{c,h}    — Shared FreeRTOS job queue (W14-H06) — kills per-action task leaks
main/heap_watchdog.{c,h}  — Periodic heap + PSRAM monitoring, logs to /heap debug endpoint
main/debug_server.{c,h}   — HTTP debug server (bearer-auth except /info /selftest);
                             count endpoints with: grep -c 'httpd_register_uri_handler' main/debug_server.c
```

### Hardware
```
main/audio.c              — ES8388 DAC via esp_codec_dev + STD TX / TDM RX I2S
main/mic.c                — ES7210 quad-mic via esp_codec_dev
main/camera.{c,h}         — esp_video V4L2 stack, SC202CS sensor, MMAP capture
main/imu.c                — BMI270 IMU via I2C
main/sdcard.{c,h}         — SDMMC 4-bit, FAT32, coexists with Wi-Fi SDIO
main/wifi.{c,h}           — Wi-Fi stack wrapper (STA/AP, reconnect, country code)
main/settings.{c,h}       — NVS-backed settings (see "NVS Settings Keys" table for full list)
```

### Dragon voice link
```
main/voice.{c,h}          — Voice WS client (port 3502), mic capture, TTS playback,
                             dictation, reconnect watchdog, three-tier mode, tool events,
                             rich-media handlers. Tab5 talks to Dragon only via this path.
main/mode_manager.{c,h}   — Voice-pipeline coordinator (IDLE ↔ VOICE). Thin mutex
                             wrapper around voice_connect/disconnect kept because
                             ui_voice / ui_notes / service_dragon switch from
                             multiple tasks.
```

The old CDP browser-streaming stack (dragon_link, mdns_discovery, touch_ws,
udp_stream, mjpeg_stream) was deleted in #155 — see that PR for removal
rationale. Tab5 now reaches Dragon only via the voice WS.

### UI framework
```
main/ui_core.{c,h}        — LVGL display init, screen management, root screen
main/ui_theme.{c,h}       — v5 Material Dark theme tokens (colors, radii, typography)
main/ui_port.h            — Port-abstraction typedefs (LV_EXT*, color shims)
main/ui_focus.{c,h}       — Focus-ring helpers for touch + remote navigation
main/ui_feedback.{c,h}    — Haptic-style visual feedback (flash, shake, toast)
```

### UI screens + overlays
```
main/ui_splash.{c,h}      — Boot animation
main/ui_home.{c,h}        — Home screen (v4·C Ambient Canvas: clock, orb, greeting, mode pill,
                             nav sheet, widget slots)
main/ui_voice.{c,h}       — Voice overlay (orb, LISTENING / DICTATION, chat bubbles, stop)
main/ui_chat.{c,h}        — Chat overlay (iMessage-style, rich-media bubbles, tool indicators)
main/chat_header.{c,h}    — Chat top bar (mode badge, session switcher, new-chat button)
main/chat_input_bar.{c,h} — Chat bottom bar (keyboard, mic, camera)
main/chat_msg_store.{c,h} — In-memory chat buffer (persists across close/open)
main/chat_msg_view.{c,h}  — Message renderer (text / media / card / audio clip)
main/chat_session_drawer.{c,h} — Session list drawer
main/chat_suggestions.{c,h}    — Quick-reply suggestions strip
main/ui_settings.{c,h}    — Settings fullscreen overlay (Display, Network, Voice, Storage,
                             Battery, OTA, About)
main/ui_wifi.{c,h}        — Wi-Fi setup (scan, select, password entry)
main/ui_keyboard.{c,h}    — On-screen keyboard (shared)
main/ui_camera.{c,h}      — Camera viewfinder (1280x720, capture to SD, gallery)
main/ui_files.{c,h}       — SD file browser (directories, WAV playback, image preview)
main/ui_notes.{c,h}       — Notes screen (search, compact cards, edit overlay, dragon sync)
main/ui_sessions.{c,h}    — Session browser (cross-session history)
main/ui_memory.{c,h}      — Memory facts browser
main/ui_agents.{c,h}      — Agents / TinkerClaw status panel
main/ui_audio.{c,h}       — Audio settings (volume, mute, routing)
main/ui_mode_sheet.{c,h}  — Three-tier voice-mode picker sheet
main/ui_nav_sheet.{c,h}   — Bottom nav sheet (home / chat / notes / settings / camera / files)
main/ui_onboarding.{c,h}  — First-boot tutorial flow
```

### Widgets + media
```
main/widget.h             — Widget data model (six types: live/card/list/chart/media/prompt)
main/widget_store.c       — Priority-resolved widget queue (bounded 32 entries)
main/media_cache.{c,h}    — HTTP image downloader + 5-slot PSRAM LRU (~2.9 MB),
                             JPEG decode via TJPGD, feeds chat rich media
```

### OTA
```
main/ota.{c,h}            — OTA check/apply/mark-valid via esp_https_ota with auto-rollback
partitions.csv            — OTA dual-slot partition table (ota_0 + ota_1)
LEARNINGS.md              — Institutional knowledge (MANDATORY reading before any changes)
```

## UI Screens
The Tab5 has 7 full screens + 2 overlays, managed by ui_core.c:
| Screen | File | Description |
|--------|------|-------------|
| Splash | ui_splash.c | Boot animation, shown during init |
| Home | ui_home.c | 4-page tileview (Material Dark polish) |
| Chat | ui_chat.c | Fullscreen overlay on home (iMessage-style Material Dark). Live status bar (Ready/Processing/Speaking), tappable mode badge cycles Local/Hybrid/Cloud, New Chat button, thinking + tool indicator bubbles |
| Notes | ui_notes.c | Separate lv_screen (loaded via lv_screen_load) |
| Settings | ui_settings.c | Fullscreen overlay on home (Material Dark, 55 objects) |
| Camera | ui_camera.c | SC202CS viewfinder |
| Files | ui_files.c | SD card file browser |
| Keyboard | ui_keyboard.c | On-screen keyboard overlay (shared) |
| Voice | ui_voice.c | Voice overlay (animated orb, PTT, dictation waveform) |

## Dictation Mode
- **Trigger:** Long-press mic button from any screen (or tap Record in Notes screen)
- **Flow:** `voice_start_dictation()` → sends `{"type":"start","mode":"dictate"}` → mic streams PCM → Dragon returns `stt_partial` segments → accumulated in 64KB PSRAM buffer
- **Auto-stop:** 5 seconds of silence triggers automatic stop (`DICTATION_AUTO_STOP_FRAMES = 250`)
- **Adaptive VAD:** Calibrates noise floor from ambient audio for reliable silence detection
- **Post-processing:** Dragon sends `dictation_summary` with auto-generated title + summary via LLM
- **UI:** Live waveform visualization using `voice_get_current_rms()`, transcript display

## Cloud Mode
- **Toggle:** Settings screen has a Cloud Mode switch. Persisted in NVS via `tab5_settings_get/set_cloud_mode()`.
- **Mechanism:** `voice_send_cloud_mode(enabled)` sends `{"type":"config_update","cloud_mode":true}` to Dragon over WebSocket.
- **Effect on Dragon:** Switches STT backend to `openrouter` (gpt-audio-mini) and TTS backend to `openrouter` (gpt-audio-mini with voice selection). Dragon ACKs with `config_update` containing applied config.
- **Default:** Off (local backends: Moonshine STT + Piper TTS on Dragon).

## Current Sprint: Phase 1 Complete (April 2026)

**Phase 1 — Voice Assistant + UI** is feature-complete:
- Ask mode (PTT voice → STT → LLM → TTS) — working end-to-end
- Dictation mode (long-press, auto-stop, adaptive VAD, 64KB buffer, post-processing) — working
- Text input via chat screen (`voice_send_text`) — working
- Cloud mode toggle (local ↔ OpenRouter STT+TTS) — working
- Device registration + session resume over WebSocket — working
- 7 UI screens + keyboard overlay + voice overlay — all functional
- NVS settings persistence (WiFi, Dragon, volume, brightness, cloud mode, session) — working

**Phase 2 — AEC + Wake Word (RETIRED in #162):**
- ESP-SR v2.4.0, AFE pipeline, `/wake` debug endpoint, Settings toggle,
  and the 3 MB model partition were all wired up, but the TDM slot
  mapping for the AEC reference channel never produced usable wake-word
  detection. The feature was parked for months and then removed in #162
  so the ~450 LoC + `esp-sr` component + partition weren't a permanent
  maintenance drag.
- Revival path: restore the `afe.{c,h}` + voice.c/voice.h wake-word stubs
  from git history (pre-#162), re-add `esp-sr` to `CMakeLists.txt` +
  `idf_component.yml`, and procure a custom "Hey Tinker" WakeNet model
  + tone-test the TDM slots to resolve the original blocker.

**Phase 2 — Remaining priorities:**
1. Desktop SDL2 simulator for fast dev loop
2. OPUS audio encoding (16kbps vs 256kbps PCM)
3. OTA firmware updates

## Recovery & Rollback

There are three layers of recovery, cheapest first. Try them in order.

### 1. On-device OTA rollback (Tab5 currently boots)
The firmware runs two OTA slots (`ota_0` / `ota_1`) and `esp_https_ota` with
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. If the new image boots but the
"self-test" phase fails (never calls `esp_ota_mark_app_valid_cancel_rollback`),
the bootloader automatically swaps back on the next reboot.

Manual trigger from a workstation:
```bash
curl -sS -H "Authorization: Bearer $TAB5_DEBUG_TOKEN" \
     -X POST http://<tab5-ip>:3500/ota/rollback
# ↑ forces a reboot into the inactive slot
```

### 2. Reflash a known-good image (Tab5 bricked)
Re-flash the currently deployed Dragon build (it's always the last known-good
binary served via `/api/ota/firmware.bin`):
```bash
cd ~/projects/TinkerTab
# Grab whichever binary Dragon is advertising as current:
curl -sS "http://192.168.1.91:3502/api/ota/check?current=0.0.0" \
     -H "Authorization: Bearer $DRAGON_API_TOKEN"
# Then either OTA it, or serial-flash directly:
idf.py -p /dev/ttyUSB0 flash
```

### 3. Git rollback (code regressed)
TinkerTab's git history IS the backup — PRs land as squash-merges on `main`,
so every feature has exactly one revert-able commit. To undo the last
landed feature:
```bash
cd ~/projects/TinkerTab
git log --oneline -5
git revert <sha>        # creates a forward commit; preserves history
idf.py build && idf.py -p /dev/ttyUSB0 flash
```

Before touching hardware in the middle of risky work, cut a snapshot branch
(`git branch snapshot/$(date +%Y%m%d-%H%M%S)`) rather than relying on
stashes — they silently evaporate on `git clean` or branch deletion.

> Historical: the old manual "physical backup" path
> (`/home/rebelforce/projects/TinkerTab-backups/rollback-20260331-132117`,
> `backup/pre-rollback-20260331-132117`, stashes) is retired as of
> wave 14 L10. Don't add to it — use the three layers above.

## Widget Platform (April 2026) — Skills Surface on Tab5

**Spec:** [`docs/WIDGETS.md`](./docs/WIDGETS.md)
**Plan:** [`docs/PLAN-widget-platform.md`](./docs/PLAN-widget-platform.md)
**Mockups:** [`.superpowers/brainstorm/widget-platform/`](./.superpowers/brainstorm/widget-platform/)

A new extensibility layer. Skills on Dragon emit typed widget state; Tab5
renders it opinionatedly. Instead of editing C and reflashing for every new
feature, **new features become Python files on Dragon** that emit structured
state into one of six widget slots. Tab5 renders in v5 style.

### What this is
- Six widget types: `live`, `card`, `list`, `chart`, `media`, `prompt`
- New WS messages: `widget_live`, `widget_live_update`, `widget_live_dismiss`,
  `widget_card`, `widget_list`, `widget_chart`, `widget_prompt`, `widget_dismiss`,
  `widget_action` (Tab5→Dragon), `widget_capability` (Tab5→Dragon)
- Single in-memory widget store in PSRAM, bounded at 32 entries
- Home `s_poem_label` promoted to a **priority-resolved live slot** — a live
  widget's title+body replaces the poem; its tone drives orb color + breathing
- Reference skill: **Time Sense** (AI-first Pomodoro) in `dragon_voice/tools/`

### Design rules (non-negotiable)
1. One live widget at a time, full stop.
2. Skills never write layout — widget vocabulary IS the layout contract.
3. No interpreter on Tab5; no JS; no Lua. Skills run on the brain only.
4. Tab5 renders opinionatedly from v5 theme tokens. Skills can't override colors.
5. Every interactive element ≥44 pt tap target; all animations 150–300 ms.
6. Unknown widget types are ignored (forward-compat); unknown icons hidden.
7. Under SDIO TX pressure, action events are rate-limited at 4/sec.

### Key files (phase 1)
- `main/widget.h` / `main/widget_store.c` — data model + priority queue
- `main/ui_home.c` — live-slot integration (extends `s_poem_label`)
- `main/voice.c` — WS handlers for `widget_*` + `widget_action` TX
- Dragon-side: `dragon_voice/surfaces/base.py` + `dragon_voice/tools/timesense_tool.py`

### When to use a widget vs a native screen
- **Widget:** skill-emitted state (timer, notification, ask-a-question)
- **Native screen:** device-specific hardware feature (camera viewfinder,
  keyboard, voice overlay) — these stay in C and don't become skills.

