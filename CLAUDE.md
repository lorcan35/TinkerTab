# TinkerTab — ESP32-P4 Tab5 Firmware

## Overview
TinkerTab is the ESP32-P4 firmware for the M5Stack Tab5, part of the TinkerClaw AI device. It connects to a Dragon server (Radxa Zero 3W) for browser streaming, voice AI, and device management.

Companion repo: [TinkerBox](https://github.com/lorcan35/TinkerBox) (Dragon-side server)

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

## Build & Flash
```bash
source ~/esp/esp-idf-v5.4.2/export.sh
idf.py build
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
- **Use IDF v5.4.2** — MIPI-DSI broken in v5.5.x
- Tab5 camera is SC202CS at SCCB 0x36 (NOT SC2336 at 0x30)
- SD card uses SDMMC SLOT 0 with LDO channel 4

## Key Technical Notes
- ESP-IDF WS transport masks frames in-place — NEVER pass string literals to `esp_transport_ws_send_raw()`, always copy to mutable buffer first
- LVGL objects must not be accessed from background tasks after screen destroy — use `volatile bool s_destroying` guards
- All FreeRTOS tasks doing network + LVGL callbacks need minimum 8KB stack on ESP32-P4
- Screenshot handler must copy framebuffer under LVGL lock, then stream without lock
- `lv_screen_load_anim()` with auto_delete=false when returning to existing home screen

## Key Files
```
main/voice.c      — Voice streaming (WS client, mic capture, TTS playback)
main/audio.c      — ES8388 DAC + shared I2S bus setup
main/mic.c        — ES7210 quad-mic via esp_codec_dev
main/dragon_link.c — Dragon discovery + connection
main/mode_manager.c — Mode FSM (IDLE/STREAMING/VOICE/BROWSING)
main/config.h     — All pin definitions and constants
LEARNINGS.md      — Institutional knowledge (MANDATORY)
```
