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
1. Desktop SDL2 simulator + dev tooling (highest ROI — test UI without flashing)
2. VAD + AEC + Wake Word ("Hey Tinker") — makes it hands-free, THE product feature
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

## MANDATORY: Check LEARNINGS.md First
Before writing any fix, CHECK LEARNINGS.md first. Your bug might already be documented. Every bug found, every fix, every gotcha MUST be added to LEARNINGS.md with Date/Symptom/Root Cause/Fix/Prevention.

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

## SIM-FIRST WORKFLOW (MANDATORY)
**Before flashing hardware, the simulator MUST pass:**
```bash
cd /home/rebelforce/projects/TinkerTab/sim
make
./tinkeros_sim --test   # must show "ALL SELF-TESTS PASSED"
./tinkeros_sim          # manual visual check
```
Only after simulator passes → flash hardware.

## Desktop SDL2 Simulator
- **Location:** `sim/` directory
- **Build:** `cd sim && make && ./tinkeros_sim`
- **Test mode:** `./tinkeros_sim --test` (self-test + exit)
- **Platform define:** `TINKEROS_SIMULATOR=1` activates simulator path in `ui_port.h`
- **HAL stubs:** `sim/stubs.c` — all hardware returns safe defaults
- **Port headers:** `sim/port/` — shadows ESP-IDF headers for compilation
- **Window:** 720×1280, mouse = touch, ESC/Q = quit

## Wokwi ESP32-P4 (Secondary Simulator)
- VS Code extension: `wokwi.wokwi-vscode` — **Linux ARM64 confirmed supported**
- License: **Paid** — Hobby+ $12/month minimum. No free tier for VS Code extension.
- **MIPI-DSI: YES** — `wokwi/esp32p4-mipi-dsi-panel-demo` confirms LVGL on ILI9881C panel
- **PSRAM cap: 8MB max** — Tab5 has 32MB. OOM likely for PSRAM-heavy framebuffers.
- **GT911 touch: NOT available** — no Wokwi part for it. Use FT6206 as functional proxy.
- Board type in diagram.json: `"board-esp32-p4-preview"`
- Firmware: use `flasher_args.json` (built by `idf.py build`) — no `--preview` flag needed
- Project files: `wokwi.toml` + `diagram.json` at repo root (already created, refs #52)
- **CLI on DGX:** `wokwi-cli-linuxstatic-arm64` binary — works headless for CI
- **Assessment:** SDL2 is primary (free, 32MB, full res, GT911). Wokwi = GDB + visual regression.
- **ESP-IDF Linux target (`idf.py set-target linux`): DOES NOT WORK** — `esp_lcd` not supported on linux target. Our SDL2 sim IS the correct approach. Do NOT attempt linux target again.

## Build & Flash
```bash
source ~/esp/esp-idf/export.sh  # v5.4.3
idf.py build
idf.py uf2        # builds UF2 at build/uf2.bin (drag-and-drop flash)
idf.py -p /dev/ttyACM0 flash
# Monitor (needs TTY — use python serial or screen):
python3 -c "import serial; s=serial.Serial('/dev/ttyACM0',115200); [print(s.readline().decode(errors='replace'),end='') for _ in range(1000)]"
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
- **I2S TDM 4-slot:** Both TX (ES8388 DAC) and RX (ES7210 ADC) share I2S_NUM_1. Both MUST use TDM 4-slot for consistent BCLK (3.072MHz at 48kHz).
- **Slot mode:** Use `I2S_SLOT_MODE_STEREO` for TDM multi-slot capture. MONO only gets slot 0.
- **Sample rates:** Hardware runs at 48kHz. Downsample 3:1 to 16kHz for STT. Upsample 1:3 from 16kHz for TTS playback.
- **esp_codec_dev:** Uses 8-bit I2C addresses (ES7210=0x80, ES8388=0x20). NOT 7-bit.
- **Mic audio:** Slot 0 = MIC-L (primary). Extract from 4-ch TDM interleaved buffer.

## IDF Version
- **Use IDF v5.4.3** — MIPI-DSI broken in v5.5.x, v5.4.2 missing PSRAM XIP + TCM stack fixes
- Tab5 camera is SC202CS at SCCB 0x36 (NOT SC2336 at 0x30)
- SD card uses SDMMC SLOT 0 with LDO channel 4

## Voice Pipeline — Critical Gaps (from March 2026 audit)
- **AEC (Acoustic Echo Cancellation):** ES7210 captures 4 TDM channels: [MIC-L, AEC, MIC-R, MIC-HP]. We only use slot 0 (MIC-L). Slot 1 = AEC reference. Without AEC, Tab5 hears its own TTS → hallucination loop. Use ESP-SR AEC.
- **VAD (Voice Activity Detection):** Currently fixed 30s recording window. Need silero-vad on Dragon to detect speech end dynamically.
- **Wake Word:** Currently push-to-talk only. Need ESP-SR WakeNet or Porcupine on Tab5 for "Hey Tinker". Tab5-side KWS = lowest latency.
- **Barge-in:** Impossible without AEC. KWS should interrupt TTS playback and reset pipeline.
- **OPUS encoding:** 16kHz PCM = 256kbps over WiFi. OPUS would cut to ~16kbps. Phase 2.

## Key Technical Notes
- ESP-IDF WS transport masks frames in-place — NEVER pass string literals to `esp_transport_ws_send_raw()`, always copy to mutable buffer first
- LVGL objects must not be accessed from background tasks after screen destroy — use `volatile bool s_destroying` guards
- All FreeRTOS tasks doing network + LVGL callbacks need minimum 8KB stack on ESP32-P4
- Screenshot handler must copy framebuffer under LVGL lock, then stream without lock
- `lv_screen_load_anim()` with auto_delete=false when returning to existing home screen

## WebSocket Protocol (Tab5 = Client Side)
See TinkerBox `docs/protocol.md` for the full spec. Tab5 responsibilities:

### Currently Implemented (voice.c)
1. **Voice Input:** `{"type":"start"}` -> binary PCM frames -> `{"type":"stop"}`
2. **Voice Cancel:** `{"type":"cancel"}` — abort current processing
3. **Keepalive:** `{"type":"ping"}` — JSON heartbeat every 15s during processing
4. **Receive:** Handle `session_start`, `stt`, `llm`, `tts_start`, binary TTS, `tts_end`, `error`

### Not Yet Implemented (planned per protocol.md)
5. **Device Registration:** On WS connect, send `{"type":"register", device_id, ...}` as first text frame. (Issue #43)
6. **Session Resume:** Store `session_id` from `session_start` in NVS. Send on reconnect.
7. **Text Input:** `{"type":"text","content":"..."}` — skips STT, same conversation.
8. **Recording:** `{"type":"record_start"}` -> binary PCM -> `{"type":"record_stop"}` — creates a note.
9. **Config Sync:** Handle `{"type":"config_update"}` from Dragon, apply settings, ACK.
10. **SD Card:** Save raw WAV to SD before/during WS send. Offline queue if Dragon unreachable. (Issue #44)

## Key Files
```
main/voice.c           — Voice streaming (WS client, mic capture, TTS playback)
main/audio.c           — ES8388 DAC + shared I2S TDM bus setup
main/mic.c             — ES7210 quad-mic via esp_codec_dev
main/dragon_link.c     — Dragon mDNS discovery + connection state machine
main/mode_manager.c    — Mode FSM (IDLE/STREAMING/VOICE/BROWSING)
main/config.h          — All pin definitions and constants
main/service_registry.c — Service lifecycle management
main/ui_voice.c        — Voice UI overlay (animated orb, PTT button)
main/ui_home.c         — TinkerOS home screen (4-page tileview)
main/settings.c        — NVS persistence (WiFi, Dragon host, volume, brightness)
main/debug_server.c    — HTTP debug server (port 8080, screenshots, touch inject)
main/main.c            — Boot sequence, serial command loop
LEARNINGS.md           — Institutional knowledge (MANDATORY)
```
