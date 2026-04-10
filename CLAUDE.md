# TinkerTab — TinkerOS Firmware (ESP32-P4 Tab5) — THE FACE

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
- **STOP investing in Dragon Mode streaming.** The product is the voice assistant.
- **Double down on voice** — that's what differentiates from every IoT display.
- Memory/RAG via Qwen3-Embedding 0.6B on Dragon. AI that remembers you.
- Skill store is the growth flywheel. Tinkerers publish, normies install.

### What to Steal from M5Stack
- Desktop SDL2 simulator build pattern (from M5Tab5-UserDemo)
- sherpa-onnx for KWS + silero-vad for VAD (both CPU ONNX, hardware-agnostic)
- Toast notification pattern, spring animation concept (write our own spring_anim.c)

### What NOT to Do
- Don't adopt Mooncake (our service_registry + mode_manager is better for embedded)
- Don't rewrite to C++
- Don't build a full browser on Tab5
- Don't over-optimize Dragon Mode streaming

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
- **Host:** 192.168.70.242
- **User:** radxa (NOT rock — user was migrated)
- **Password:** radxa
- **SSH:** `sshpass -p 'radxa' ssh radxa@192.168.70.242`
```bash
sshpass -p 'radxa' ssh radxa@192.168.70.242
echo radxa | sudo -S systemctl status tinkerclaw-voice
echo radxa | sudo -S journalctl -u tinkerclaw-voice --no-pager -n 50
```

### Dragon Stability
- **Ethernet only** — WiFi disabled (`nmcli radio wifi off`), DHCP IP 192.168.70.242 on enp1s0
- **Stripped services** — gdm3, snapd, ollama, nanobot, rustdesk, fwupd all masked. Only tinkerclaw-* services run.
- **ngrok tunnel** — `tinkerclaw-ngrok.service` maintains `tinkertab.ngrok.dev` → localhost:3502
- **Tab5 ngrok fallback** — voice.c tries local WS first, falls back to wss://tinkertab.ngrok.dev:443

## Build & Flash
```bash
# Always use ESP-IDF v5.4.3 — matches dependencies.lock
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
**Note:** Tab5 IP is DHCP-assigned. Current lease: 192.168.70.128. Update if it changes.

```bash
# Display
curl -s -o screen.bmp http://192.168.70.128:8080/screenshot     # BMP screenshot
curl -s http://192.168.70.128:8080/info | python3 -m json.tool   # Device info JSON

# Touch
curl -s -X POST http://192.168.70.128:8080/touch -d '{"x":360,"y":640,"action":"tap"}'

# Settings (read all NVS settings as JSON)
curl -s http://192.168.70.128:8080/settings | python3 -m json.tool

# Voice Mode (switch Local/Hybrid/Cloud remotely)
curl -s -X POST "http://192.168.70.128:8080/mode?m=0"  # Local
curl -s -X POST "http://192.168.70.128:8080/mode?m=1"  # Hybrid (cloud STT+TTS)
curl -s -X POST "http://192.168.70.128:8080/mode?m=2&model=anthropic/claude-sonnet-4-20250514"  # Full Cloud

# Navigation (force screen change — bypasses tileview)
curl -s -X POST "http://192.168.70.128:8080/navigate?screen=settings"
curl -s -X POST "http://192.168.70.128:8080/navigate?screen=notes"
curl -s -X POST "http://192.168.70.128:8080/navigate?screen=chat"
curl -s -X POST "http://192.168.70.128:8080/navigate?screen=camera"
curl -s -X POST "http://192.168.70.128:8080/navigate?screen=home"

# Camera (capture live frame as BMP)
curl -s -o frame.bmp http://192.168.70.128:8080/camera

# OTA
curl -s http://192.168.70.128:8080/ota/check | python3 -m json.tool
curl -s -X POST http://192.168.70.128:8080/ota/apply

# Wake word (toggle AFE always-listening)
curl -s -X POST http://192.168.70.128:8080/wake

# Chat (send text to Dragon via voice WS)
curl -s -X POST http://192.168.70.128:8080/chat -d '{"text":"What time is it?"}'

# Voice state (connected, state_name, last_llm_text, last_stt_text)
curl -s http://192.168.70.128:8080/voice | python3 -m json.tool

# Force voice WS reconnect
curl -s -X POST http://192.168.70.128:8080/voice/reconnect

# Self-test (8-point subsystem check: WiFi, Dragon, voice WS, display, audio, SD, camera, IMU)
curl -s http://192.168.70.128:8080/selftest | python3 -m json.tool
```

## ESP32-P4 Memory Rules
- **PSRAM for large buffers (>4KB):** Use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`. Static BSS arrays eat limited internal SRAM (~512KB shared with FreeRTOS).
- **Always free PSRAM:** Use `heap_caps_free()`, not `free()`.
- **No vTaskDelete(NULL):** Use `vTaskSuspend(NULL)` — P4 TLSP cleanup crash (issue #20).
- **Stack sizes:** SDIO tasks need 8K+. Mic task 4K (TDM buffer in PSRAM). WS task 8K.
- **PSRAM cache coherency:** DPI DMA reads PSRAM directly. Call `esp_cache_msync()` after CPU writes to framebuffer.

## Audio Pipeline Rules
- **I2S mixed mode:** TX = STD Philips (ES8388 DAC), RX = TDM 4-slot (ES7210 ADC), both on I2S_NUM_1. Matches M5Stack BSP exactly. ESP32-P4 supports mixed STD/TDM on same port.
- **ES8388 init:** NEVER write custom registers. Use `es8388_codec_new()` from esp_codec_dev library. Custom init had 5 register differences that prevented audio (issue #46).
- **Playback:** All audio via `esp_codec_dev_write()` through `tab5_audio_play_raw()`. Playback drain task (voice.c) handles producer-consumer I2S writes.
- **Slot mode:** Use `I2S_SLOT_MODE_STEREO` for TDM multi-slot capture. MONO only gets slot 0.
- **Sample rates:** Hardware runs at 48kHz. Downsample 3:1 to 16kHz for STT. Upsample 1:3 from 16kHz for TTS playback.
- **esp_codec_dev:** Uses 8-bit I2C addresses (ES7210=0x80, ES8388=0x20). NOT 7-bit.
- **Mic audio:** Slot 0 = MIC-L (primary). Extract from 4-ch TDM interleaved buffer.

## IDF Version
- **Use IDF v5.4.3** — matches `dependencies.lock`. Build always requires `idf.py set-target esp32p4` after any clean build or target change. Run `idf.py fullclean build` when in doubt.
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
Settings dropdown: **Local / Hybrid / Full Cloud**

| Mode | STT | LLM | TTS | Latency | Cost |
|------|-----|-----|-----|---------|------|
| Local (0) | Moonshine | NPU/Ollama (default: qwen3:1.7b, 7.1 tok/s) | Piper | 2-5s | Free |
| Hybrid (1) | OpenRouter gpt-audio-mini | Local (unchanged) | OpenRouter gpt-audio-mini | 4-8s | ~$0.02/req |
| Full Cloud (2) | OpenRouter gpt-audio-mini | User-selected (Haiku/Sonnet/GPT-4o) | OpenRouter gpt-audio-mini | 3-6s | $0.03-0.08/req |

- **LLM Model Picker:** Settings dropdown: Local NPU / Local Ollama / Claude Haiku / Claude Sonnet / GPT-4o mini (enabled only in Full Cloud mode)
- **Auto-Fallback:** If cloud STT/TTS fails, Dragon falls back to local for that request + sends `config_update` with `error` field → Tab5 auto-reverts to Local mode
- **NVS:** `voice_mode` (uint8: 0/1/2), `llm_model` (string: model ID)
- **Protocol:** `{"type":"config_update","voice_mode":0|1|2,"llm_model":"anthropic/claude-sonnet-4-20250514"}`

## OTA Firmware Updates
- **Dual OTA partitions:** ota_0 (3MB) + ota_1 (3MB) with otadata boot selector
- **Check:** `tab5_ota_check()` → GET `http://dragon:3502/api/ota/check?current=0.6.0`
- **Apply:** `tab5_ota_apply(url)` → downloads via `esp_https_ota()`, writes inactive slot, reboots
- **Auto-rollback:** New firmware boots in PENDING_VERIFY. If crash before `tab5_ota_mark_valid()`, bootloader reverts.
- **Settings UI:** "Check Update" button shows version + partition. "Apply Update" green button appears when update available.
- **Debug:** `/ota/check` and `/ota/apply` endpoints on debug server
- **Deploy new firmware:**
  ```bash
  scp build/tinkertab.bin radxa@192.168.70.242:/home/radxa/ota/
  echo '{"version":"0.6.1","sha256":""}' | ssh radxa@192.168.70.242 'cat > /home/radxa/ota/version.json'
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

### Remaining Gaps
- **AEC + Wake Word:** ESP-SR integrated, AFE runs, but wake word detection not triggering (TDM slot mapping issue). Parked.
- **OPUS encoding:** 16kHz PCM = 256kbps. OPUS would cut to ~16kbps. Future optimization.

## Key Technical Notes
- ESP-IDF WS transport masks frames in-place — NEVER pass string literals to `esp_transport_ws_send_raw()`, always copy to mutable buffer first
- LVGL objects must not be accessed from background tasks after screen destroy — use `volatile bool s_destroying` guards
- All FreeRTOS tasks doing network + LVGL callbacks need minimum 8KB stack on ESP32-P4
- Screenshot handler must copy framebuffer under LVGL lock, then stream without lock
- `lv_screen_load_anim()` with auto_delete=false when returning to existing home screen
- sdkconfig changes always require `idf.py fullclean build` — incremental builds cache stale config

### Key Fixes (April 2026)
- **Settings WDT crash fixed:** `f_getfree()` on a 128GB SD card blocks the LVGL thread for ~30s, triggering the watchdog. Fix: cache the `f_getfree` result from boot, feed `esp_task_wdt_reset()` between settings UI sections during creation.
- **Response timeout (local vs cloud):** Local mode needs 5 min timeout for tool-calling chains (small models are slow). Cloud mode keeps 35s timeout. Timeout is mode-aware.
- **Tool parser tolerant of small model quirks:** qwen3:1.7b adds stray `>` after `</args>`, sometimes omits closing tags. Parser uses tolerant regex with fallback patterns to handle these gracefully.
- **Speaker buzzing fixed:** IO expander P1 (SPK_EN) now initialized LOW at boot to prevent speaker buzzing on startup.
- **Settings rewrite:** Replaced with fullscreen overlay using manual Y positioning. No flex layout, no separate screen. Eliminates WDT crash + draw buffer exhaustion.
- **WiFi:** Switched to DHCP, WPA2-PSK router, lowered auth threshold.
- **Voice WS:** Added TLS cert bundle for ngrok, fixed TCP/SSL transport leaks.
- **LVGL memory:** 128KB pool + 64KB expand (was 64KB, caused draw pipeline crashes).

## WebSocket Protocol (Tab5 = Client Side)
See TinkerBox `docs/protocol.md` for the full spec. Tab5 responsibilities:

### Tab5 → Dragon (sending)
1. **Voice Input:** `{"type":"start"}` (with optional `"mode":"dictate"`) -> binary PCM frames -> `{"type":"stop"}`
2. **Voice Cancel:** `{"type":"cancel"}` — abort current processing
3. **Keepalive:** `{"type":"ping"}` — JSON heartbeat every 15s during processing
4. **Text Input:** `{"type":"text","content":"..."}` — skips STT, goes straight to LLM
5. **Config Update:** `{"type":"config_update","voice_mode":0|1|2,"llm_model":"..."}` — three-tier mode switch. Backward compat: `cloud_mode` bool still accepted.
6. **Device Registration:** `{"type":"register","device_id":"...","session_id":"..."}` on WS connect
7. **Clear History:** `{"type":"clear_history"}` — reset conversation context

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

### Not Yet Implemented (planned)
- **Recording:** `{"type":"record_start"}` -> binary PCM -> `{"type":"record_stop"}` — creates a note.
- **SD Card:** Save raw WAV to SD before/during WS send. Offline queue if Dragon unreachable. (Issue #44)

## Key Files
```
main/voice.c           — Voice WS client, mic capture, TTS playback, dictation, reconnect watchdog, three-tier mode, tool event handling (tool_call/tool_result)
main/voice.h           — Voice API: connect, listen, dictate, cancel, mode switch, reconnect watchdog
main/ota.c             — OTA: check Dragon for updates, download via esp_https_ota, auto-rollback
main/ota.h             — OTA API: tab5_ota_check(), tab5_ota_apply(), tab5_ota_mark_valid()
main/camera.c          — Camera: esp_video V4L2 stack, SC202CS sensor, MMAP capture, exposure tuning. V4L2 format string issues resolved (cast __u32 to unsigned long for %lu/%lx).
main/camera.h          — Camera API: init, capture, save_jpeg, set_resolution
main/afe.c             — ESP-SR Audio Front End wrapper (AEC + WakeNet9, parked)
main/audio.c           — ES8388 DAC via esp_codec_dev + STD TX / TDM RX I2S
main/mic.c             — ES7210 quad-mic via esp_codec_dev
main/dragon_link.c     — Dragon mDNS discovery + CDP connection state
main/mode_manager.c    — Mode FSM (IDLE/STREAMING/VOICE/BROWSING), voice WS kept across transitions
main/config.h          — Pin definitions, constants, OTA paths, firmware version (v0.6.0)
main/settings.c        — NVS: WiFi, Dragon host, volume, brightness, voice_mode, llm_model, session_id
main/settings.h        — Settings API including three-tier voice_mode + llm_model
main/ui_voice.c        — Voice overlay (orb, LISTENING/DICTATION label, chat bubbles, stop button)
main/ui_home.c         — Home screen (clock, orb, Ask Tinker, Camera, Files, notes card, nav bar)
main/ui_chat.c         — Chat overlay (text conversation, mic button, message persistence across close/open)
main/ui_notes.c        — Notes screen (search, compact cards, edit overlay, voice/text, SD storage, Dragon sync)
main/ui_settings.c     — Settings fullscreen overlay with manual Y positioning (no flex layout, no separate screen). Sections: Display, Network+WiFi+Dragon host, Voice mode+model, Storage, Battery, OTA, About. Uses voice_send_config_update(mode, model) for full three-tier config_update (integer voice_mode + llm_model string, not boolean bridge).
main/ui_camera.c       — Camera viewfinder (1280x720 canvas, capture to SD, resolution picker, gallery)
main/ui_files.c        — SD card file browser (directories, WAV playback, image preview)
main/ui_wifi.c         — WiFi setup (scan, select, password entry)
main/ui_keyboard.c     — On-screen keyboard overlay (shared by all text inputs)
main/ui_core.c         — LVGL display init, screen management, theme
main/debug_server.c    — HTTP debug server (12 endpoints: info, screenshot, touch, settings, mode, navigate, camera, ota, wake, etc.)
main/main.c            — Boot sequence: hardware init → WiFi → Dragon link → LVGL → voice auto-connect → watchdog
main/imu.c             — BMI270 IMU via I2C
partitions.csv         — OTA dual-slot partition table (ota_0 + ota_1, 3MB each)
LEARNINGS.md           — Institutional knowledge (MANDATORY reading before any changes)
```

## UI Screens
The Tab5 has 7 full screens + 2 overlays, managed by ui_core.c:
| Screen | File | Description |
|--------|------|-------------|
| Splash | ui_splash.c | Boot animation, shown during init |
| Home | ui_home.c | 4-page tileview (main launcher) |
| Chat | ui_chat.c | Text conversation with Tinker (user/assistant bubbles) |
| Notes | ui_notes.c | Voice dictation + typed notes, SD card backed |
| Settings | ui_settings.c | Fullscreen overlay with manual Y positioning (not a separate screen). WiFi, Dragon host, brightness, volume, voice mode |
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

**Phase 2 — AEC + Wake Word (IN PROGRESS):**
- ESP-SR v2.4.0 integrated, compiles and links on ESP32-P4
- AFE pipeline: AEC + NS + VAD + WakeNet9 — runs stable (1-mic "MR" mode, LOW_COST)
- afe.c/afe.h wrapper, Settings toggle, /wake debug endpoint — all wired
- 3MB model partition with wn9_hiesp + wn9_hilexin WakeNet models flashed
- **BLOCKER:** Wake word detection not triggering — likely TDM slot mapping issue (AEC ref on wrong channel cancels real voice). Needs physical slot verification test + possibly custom "Hey Tinker" model from Espressif.
- PTT mode still works as before when wake word is OFF (default).

**Phase 2 — Remaining priorities:**
1. Fix wake word TDM slot mapping (tone test to identify AEC ref channel)
2. Custom "Hey Tinker" WakeNet model (contact Espressif sales@espressif.com)
3. Desktop SDL2 simulator for fast dev loop
4. OPUS audio encoding (16kbps vs 256kbps PCM)
5. OTA firmware updates

## Recovery & Rollback
If something breaks after a build/flash:
- Physical backup: `/home/rebelforce/projects/TinkerTab-backups/rollback-20260331-132117`
- Backup git branch: `backup/pre-rollback-20260331-132117`
- Stash: `stash@{0}` holds uncommitted work before rollback
- To restore pre-rollback work: `git switch main && git stash apply stash@{0}`
- Safe rollback branch: `rollback/e7c7253-clean-20260331` at commit `e7c7253`
