# TinkerTab Session State

> **Last updated:** 2026-03-27 ~19:00 GST
> **Last session:** Voice UI + Phase 1 "Box → Product" + stability fixes
> **Device state:** Tab5 flashed with latest (528fd4a), Dragon unplugged

---

## What Was Done (Mar 27)

### Voice UI (ui_voice.c ~600 lines)
- Floating mic button (72px, bottom-left, cyan dot)
- Full-screen overlay with 5 animated states: IDLE → CONNECTING → LISTENING → PROCESSING → SPEAKING
- Animated orbs (cyan breathing / purple pulsing / green expanded), transcript display, wave bars
- Stop button, recording timer, tap-to-cancel, background dismiss
- Full pipeline working: mic → WebSocket → Dragon (Whisper STT → Ollama LLM → Piper TTS) → speaker

### Phase 1 — "Box → Product"
- Dragon services deployed: tinkerclaw-stream (3501), tinkerclaw-voice (3502), tinkerclaw-mdns (avahi)
- nginx proxies: Ollama on :3503, CDP on :3504
- mDNS auto-discovery on Tab5 (no more hardcoded Dragon IP)
- WiFi config screen (scan, tap, password with on-screen keyboard)
- NVS settings persistence (WiFi, Dragon host, brightness, volume)
- HTTP debug server on Tab5 :8080 (/screenshot, /info, /touch, /reboot)

### Bug Fixes (17 total across session)
- **LVGL thread safety** — voice_set_state() called UI from Core 1 without mutex (13 crash paths). Fix: recursive mutex + ui_lock/unlock.
- **WS send crash** — ESP-IDF masks frames in-place, string literals in flash = Store access fault. Fix: heap buffer copy.
- **Stack overflows** — keyboard+voice overlays too many LVGL objects. Fix: lazy init + bumped all task stacks to 8KB.
- **WiFi screen watchdog panic** — lv_screen_load_anim() infinite loop with tileview. Fix: use lv_screen_load() instead.
- **Screenshot handler blocking** — held LVGL mutex during network streaming. Fix: copy framebuffer under lock, stream without lock.
- rx_buf off-by-one overflow, transport leak, disconnect UAF, double-connect race, speaker amp enable/disable, transcript callback, stereo-to-mono 6dB fix, response timeout 30s, max recording 30s

### UI Overhaul
- 117 sizing edits across 10 files for 720x1280 DPI
- Navigation + screen transitions (no more dead-end screens)
- Touch feedback on all tappable elements
- "Coming Soon" toast for unbuilt features (Audio, AI Chat)
- App grid icons wired to Settings/Camera/Files

---

## What's Broken / Open

### Critical
- **Voice "blank audio"** — user reported mic tap → "blank audio" → disconnect → back to home. Needs Dragon SSH to diagnose (Dragon was offline when we tried).
- **MJPEG DMA decode** — ESP_ERR_NOT_ALLOWED spam. Pre-existing framebuffer race between MJPEG stream and LVGL. Not a quick fix.

### Open GitHub Issues (lorcan35/TinkerTab)
- #1 — Voice pipeline crash (RESOLVED in cb65f43, needs closing)
- #2 — Core dumps disabled (RESOLVED in cb65f43, needs closing)
- #3 — Screenshot mutex (RESOLVED in cb65f43, needs closing)
- #8 — WiFi app crash (RESOLVED in 528fd4a, needs closing)

### Uncommitted Local Changes
- `main/main.c` — UAE timezone (GST-4) at boot
- `main/rtc.c` — NTP sync uses localtime_r, logs GST

---

## What's Next

### Immediate (next session)
1. Commit + push timezone fix
2. Close resolved GitHub issues (#1, #2, #3, #8)
3. Power on Dragon, verify voice pipeline end-to-end
4. Debug "blank audio" issue — check Dragon logs via SSH
5. Test full voice flow: mic → transcribe → LLM → TTS → speaker playback

### Phase 2 — "Make It Smart"
- Wake word detection ("Hey Glyph" via ESP-SR WakeNet)
- Voice commands that control the OS ("open camera", "what time is it")
- Seamless LVGL ↔ Dragon mode switching
- Real weather/time data on home screen
- AI Chat screen with conversation history

### Phase 3 — "Make It Special"
- Glyph AI personality (animated onboarding, contextual responses)
- OTA firmware updates from Dragon
- Multi-device ecosystem
- App store / PingApp integration

### User Stories Still Incomplete (from 20-story audit)
- US-11: Dragon offline indicator on mic button (heartbeat ping)
- US-12: PTT feedback (confirmation tone, waveform, timer, cancel zone)
- US-13: Long processing phases ("Transcribing..." → "Thinking..." → "Preparing...")
- US-14: TTS interruption (barge-in, single-tap interrupt-and-record)
- US-15: Connection drop recovery (retry buffer, auto-reconnect)
- US-16: Voice/keyboard switching (shared text target, mode toggle)
- US-17: Voice navigation commands (action dispatcher)
- US-18: Background noise handling (SNR meter, confidence check)
- US-19: First-connect greeting (NVS flag + TTS greeting) — QUICK WIN
- US-20: Accessibility (left-hand mode, swipe dismiss, scale factor)

---

## Infrastructure

### Dragon Q6A (192.168.1.89)
- SSH: `sshpass -p 'radxa' ssh radxa@192.168.1.89` (user migrated from rock to radxa)
- Services: tinkerclaw-stream (3501), tinkerclaw-voice (3502), tinkerclaw-mdns
- Ollama models: llama3.2:3b, phi4-mini, gemma3:4b
- Storage: ~429GB free

### Tab5 (ESP32-P4)
- WiFi IP: 192.168.1.90 (Sawaya-2.4G)
- Debug server: http://192.168.1.90:8080
- Flash: `idf.py -p /dev/ttyACM0 flash`
- Monitor: `idf.py -p /dev/ttyACM0 monitor`
- ESP-IDF: v5.4.3 (NOT v5.5.x — breaks DSI)

### Repos
- **TinkerTab**: github.com/lorcan35/TinkerTab (11 open issues as of 2026-03-30)
- **TinkerBox**: github.com/lorcan35/TinkerBox (2 commits, 0 issues)
- **tinkerclaw**: ~/projects/tinkerclaw (fork of zclaw, can't push)

---

## Dev Workflow
Every bug fix: **Issue → Fix → Commit (closes #N) → Push → Verify**
See CLAUDE.md for full workflow details.
