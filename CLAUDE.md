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
- **Host:** 192.168.1.89
- **User:** radxa (NOT rock — user was migrated)
- **Password:** radxa
- **SSH:** `sshpass -p 'radxa' ssh radxa@192.168.1.89`
```bash
sshpass -p 'radxa' ssh radxa@192.168.1.89
echo radxa | sudo -S systemctl status tinkerclaw-voice
echo radxa | sudo -S journalctl -u tinkerclaw-voice --no-pager -n 50
```

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

## Debug Server
- Screenshots: `curl -s -o screen.bmp http://192.168.1.90:8080/screenshot`
- Device info: `curl -s http://192.168.1.90:8080/info | python3 -m json.tool`
- Touch inject: `curl -s -X POST http://192.168.1.90:8080/touch -d '{"x":360,"y":640,"action":"tap"}'`

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
- **Use IDF v5.5.2** — matches `dependencies.lock`. Build always requires `idf.py set-target esp32p4` after any clean build or target change. Run `idf.py fullclean build` when in doubt.
- Tab5 camera is SC202CS at SCCB 0x36 (NOT SC2336 at 0x30)
- SD card uses SDMMC SLOT 0 with LDO channel 4

## Voice Pipeline
### Implemented
- **Ask Mode:** Short-form voice: PTT → mic stream → Dragon STT → LLM → TTS playback (30s max recording)
- **Dictation Mode:** Long-form recording via `voice_start_dictation()`. Long-press mic from any screen. STT-only (no LLM/TTS). Auto-stop after 5s silence (`DICTATION_AUTO_STOP_FRAMES = 250 × 20ms`). Adaptive VAD calibration. 64KB PSRAM transcript buffer (~2hrs). Post-processing on Dragon generates title + summary (`dictation_summary` message).
- **Text Input:** `voice_send_text()` sends `{"type":"text","content":"..."}` — skips STT, same conversation context.
- **Cloud Mode Toggle:** `voice_send_cloud_mode(bool)` sends `{"type":"config_update","cloud_mode":true/false}` to Dragon, switching STT+TTS backends between local and OpenRouter.
- **Live RMS:** `voice_get_current_rms()` for waveform visualization during dictation.

### Critical Gaps (from March 2026 audit)
- **AEC (Acoustic Echo Cancellation):** ES7210 captures 4 TDM channels: [MIC-L, AEC, MIC-R, MIC-HP]. We only use slot 0 (MIC-L). Slot 1 = AEC reference. Without AEC, Tab5 hears its own TTS → hallucination loop. Use ESP-SR AEC.
- **Wake Word:** Currently push-to-talk only. Need ESP-SR WakeNet or Porcupine on Tab5 for "Hey Tinker". Tab5-side KWS = lowest latency.
- **Barge-in:** Impossible without AEC. KWS should interrupt TTS playback and reset pipeline.
- **OPUS encoding:** 16kHz PCM = 256kbps over WiFi. OPUS would cut to ~16kbps. Phase 2.

## Key Technical Notes
- ESP-IDF WS transport masks frames in-place — NEVER pass string literals to `esp_transport_ws_send_raw()`, always copy to mutable buffer first
- LVGL objects must not be accessed from background tasks after screen destroy — use `volatile bool s_destroying` guards
- All FreeRTOS tasks doing network + LVGL callbacks need minimum 8KB stack on ESP32-P4
- Screenshot handler must copy framebuffer under LVGL lock, then stream without lock
- `lv_screen_load_anim()` with auto_delete=false when returning to existing home screen
- sdkconfig changes always require `idf.py fullclean build` — incremental builds cache stale config

## WebSocket Protocol (Tab5 = Client Side)
See TinkerBox `docs/protocol.md` for the full spec. Tab5 responsibilities:

### Tab5 → Dragon (sending)
1. **Voice Input:** `{"type":"start"}` (with optional `"mode":"dictate"`) -> binary PCM frames -> `{"type":"stop"}`
2. **Voice Cancel:** `{"type":"cancel"}` — abort current processing
3. **Keepalive:** `{"type":"ping"}` — JSON heartbeat every 15s during processing
4. **Text Input:** `{"type":"text","content":"..."}` — skips STT, goes straight to LLM
5. **Config Update:** `{"type":"config_update","cloud_mode":true/false}` — toggle cloud STT+TTS on Dragon
6. **Device Registration:** `{"type":"register","device_id":"...","session_id":"..."}` on WS connect
7. **Clear History:** `{"type":"clear_history"}` — reset conversation context

### Dragon → Tab5 (receiving)
1. **session_start** — session_id for NVS persistence
2. **stt** / **stt_partial** — transcription results (partial for dictation streaming)
3. **llm** — LLM response text (streamed)
4. **tts_start** / binary TTS / **tts_end** — TTS audio playback
5. **dictation_summary** — post-processing results: `{"title":"...","summary":"..."}`
6. **pong** — keepalive response
7. **config_update** — ACK with applied backend config + cloud_mode state
8. **error** — error details

### Not Yet Implemented (planned)
- **Recording:** `{"type":"record_start"}` -> binary PCM -> `{"type":"record_stop"}` — creates a note.
- **SD Card:** Save raw WAV to SD before/during WS send. Offline queue if Dragon unreachable. (Issue #44)

## Key Files
```
main/voice.c           — Voice streaming (WS client, mic capture, TTS playback, dictation, text input, cloud mode)
main/audio.c           — ES8388 DAC via esp_codec_dev + STD TX / TDM RX I2S
main/mic.c             — ES7210 quad-mic via esp_codec_dev
main/dragon_link.c     — Dragon mDNS discovery + connection state machine
main/mode_manager.c    — Mode FSM (IDLE/STREAMING/VOICE/BROWSING)
main/config.h          — All pin definitions and constants
main/service_registry.c — Service lifecycle management
main/settings.c        — NVS persistence (WiFi, Dragon host, volume, brightness, cloud_mode, session_id)
main/ui_voice.c        — Voice UI overlay (animated orb, PTT button, dictation long-press)
main/ui_home.c         — TinkerOS home screen (4-page tileview)
main/ui_chat.c         — Chat screen (text conversation with Tinker, user/assistant message bubbles)
main/ui_notes.c        — Notes screen (voice dictation + text notes, SD card storage)
main/ui_settings.c     — Settings screen (WiFi, Dragon, brightness, volume, cloud mode toggle)
main/ui_camera.c       — Camera viewfinder screen
main/ui_files.c        — File browser (SD card)
main/ui_audio.c        — Audio test/debug screen
main/ui_splash.c       — Boot splash screen
main/ui_wifi.c         — WiFi setup/connection screen
main/ui_keyboard.c     — On-screen keyboard overlay (shared by all text-input screens)
main/ui_core.c         — LVGL display init, screen management, theme
main/debug_server.c    — HTTP debug server (port 8080, screenshots, touch inject)
main/main.c            — Boot sequence, serial command loop
main/imu.c             — BMI270 IMU via I2C (try-read-retry pattern)
LEARNINGS.md           — Institutional knowledge (MANDATORY)
```

## UI Screens
The Tab5 has 7 full screens + 2 overlays, managed by ui_core.c:
| Screen | File | Description |
|--------|------|-------------|
| Splash | ui_splash.c | Boot animation, shown during init |
| Home | ui_home.c | 4-page tileview (main launcher) |
| Chat | ui_chat.c | Text conversation with Tinker (user/assistant bubbles) |
| Notes | ui_notes.c | Voice dictation + typed notes, SD card backed |
| Settings | ui_settings.c | WiFi, Dragon host, brightness, volume, cloud mode |
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
