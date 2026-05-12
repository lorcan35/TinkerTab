# TinkerTab — TinkerOS Firmware (ESP32-P4 Tab5) — THE FACE

> **Trying to understand what this project IS rather than how to operate it?**  Start with [TinkerBox's `docs/ARCHITECTURE.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/ARCHITECTURE.md) — the canonical "what is TinkerClaw?" doc, with diagrams of how Tab5 + Dragon + TinkerClaw fit together. Then [`docs/HARDWARE.md`](docs/HARDWARE.md) for the Tab5 pinout + IC list, and [`GLOSSARY.md`](GLOSSARY.md) for any unfamiliar terms.  This file (CLAUDE.md) is the *runbook* — deploy, debug, restart, monitor.

## Active Investigations — READ FIRST before related work

- **UI/UX hardening — CLOSED** → [`docs/AUDIT-ui-ux-2026-04-29.md`](docs/AUDIT-ui-ux-2026-04-29.md) + [`docs/PLAN-ui-ux-hardening.md`](docs/PLAN-ui-ux-hardening.md) + LEARNINGS "TT #328 UI/UX hardening".
  As of 2026-04-30, 9 wave-by-wave PRs have closed 14/16 audit P0s on `feat/k144-phase6a-baud-switch`: a11y contrast, mode-array drift, mic-button leak, atomic touch injection, toast tones + persistent error banner + 4 new `error.*` obs classes, per-state voice icons, orb safe long-press + undo, universal `ui_tap_gate` debounce, chat-header touch-target lift, shared `widget_mode_dot` extract, nav-sheet 3×3 (Focus tile P0 #4), dead-API removal, onboarding Wi-Fi step, dual mode-control collapse (orb long-press → mode-sheet), and discoverability chevron + first-launch hint.  Two P0s deferred as larger scope: K144 as 5th tier in 3-dial sheet (touches autonomy-dial product semantics) + orb-overload across 4 surfaces (cross-team IA call).

- **Stability — CLOSED** → [`docs/STABILITY-INVESTIGATION.md`](docs/STABILITY-INVESTIGATION.md)
  As of 2026-04-27 the investigation is fundamentally done. Real root cause of the long-residual crash class: **LVGL 9.x `lv_async_call` is NOT thread-safe** (does `lv_malloc` + `lv_timer_create` against unprotected TLSF) — the codebase had been treating it as thread-safe for years per a wrong comment in `ui_core.c`. PR #257 hand-wrapped the empirical site (mem_fetch); PR #259 added a `tab5_lv_async_call(cb, arg)` helper in `ui_core.{h,c}` and replaced all 49 sites uniformly. Today's stress benchmark progression: 50 % uptime → 92.4 % (#246) → 96 % (#249) → 97.6 % (#251+#253+#255) → **100 %, zero reboots in 5-min mixed nav+screenshot stress** (#257+#259). **Rule for any new lv_async_call use: ALWAYS call `tab5_lv_async_call`, never the LVGL primitive directly.** If a new stability symptom surfaces, do a fresh systematic-debugging pass — the historic "whack-a-mole" + LVGL pool + SDIO RX classes are all closed, so a new crash is a new mechanism.

## External Hardware Modules — READ FIRST when extending the device

Tab5 has connectors for stackable + plug-in add-ons.  Two parallel projects scope what hooks them up; both ship as **modular addons** — Tab5 must NEVER depend on either being present.  Six modularity rules locked in each plan doc (boot-path-agnostic, no-feature-regress-when-absent, capability-detection gates everything, UI surfaces gray-out when absent, hot-unplug graceful, no `#ifdef`).

### Grove sensor support (Port A I2C — HY2.0-4P)
- Plan: [`docs/PLAN-grove.md`](docs/PLAN-grove.md) · Tracking: [#316](https://github.com/lorcan35/TinkerTab/issues/316)
- Status: parked, hardware on order.  Phase 1 = Port A I2C bring-up + EXT5V_EN pin discovery on the IO expanders.

### M5Stack LLM Module Kit (K144) — DONE 2026-04-29 (Phases 0-6 + 7-wave hardening)
- Plan: [`docs/PLAN-m5-llm-module.md`](docs/PLAN-m5-llm-module.md) · Tracking: [#317](https://github.com/lorcan35/TinkerTab/issues/317)
- **Hardware topology — Mate carrier required:** stack the **Module13.2 LLM Mate** carrier between Tab5 and K144, plug the K144's top USB-C into power.  Direct K144-on-Tab5 stack causes M5-Bus 5V rail collisions that wedge the AX630C NPU silicon (LEARNINGS: "K144 must sit on the Module13.2 LLM Mate carrier").  Stack order:  `Tab5 base → Mate (USB-C powered) → K144`.
- **Pins:**  UART_NUM_1, TX = GPIO 6, RX = GPIO 7 (Port C UART, M5-Bus pins 16/15 — Mate carrier passes these straight through to Tab5).  115200 8N1.  Avoid UART0 (G37/G38, M5-Bus 13/14) — collides with `idf.py monitor`.
- **Code:**  [`bsp/tab5/uart_port_c.{c,h}`](bsp/tab5/uart_port_c.h) (Phase 1, mutex added Wave 1) → [`main/m5_stackflow.{c,h}`](main/m5_stackflow.h) marshalling (Phase 2) → [`main/voice_m5_llm.{c,h}`](main/voice_m5_llm.h) sidecar (Phase 3, chain APIs added Phase 6b) → [`main/voice_onboard.{c,h}`](main/voice_onboard.h) lifecycle module (Wave 4b extract — owns warmup + per-text failover + autonomous chain) → chat UI bubbles + `VMODE_LOCAL_ONBOARD` + reconnect-back toast (Phase 5) → Settings UI radio row (Phase 5b) → adaptive UART baud + TTS (Phase 6a/c) → chat-overlay mic-tap integration (Phase 6b chat).
- **Phase 6 status:** UART baud verified clean to 1.5 Mbps (Phase 6a); K144 TTS verified speaking through Tab5's speaker (Phase 6c); **autonomous voice-assistant chain** (audio → asr → llm → per-utterance TTS) verified live end-to-end on hardware (Phase 6b, 2026-04-29).  User speaks at the K144 stack → on-device ASR + LLM → per-utterance TTS via `voice_m5_llm_tts` → audio bytes stream back over UART → Tab5 upsamples 1:3 to 48 kHz → plays through Tab5's speaker.  Exposed via `m5chain [seconds]` serial REPL for bench validation, AND via the chat overlay's mic orb tap when `vmode=4`.  K144's hardware mic is wired + working (RMS jumps 8× when user speaks, verified via `/opt/usr/bin/tinycap`).  See LEARNINGS "K144 voice-assistant chain runs autonomously" + "K144 has a working microphone".

- **Wave 1-7 chain hardening (TT #327, 2026-04-29):** Three parallel audits (UI/UX, piping, architecture) of the chain integration surfaced 5 P0s + ~12 P1s.  Closed via 7 wave PRs on PR #326.  Notable artifacts:
  - `bsp/tab5/uart_port_c` recursive mutex serialises every K144 UART transaction; chain_run takes/releases per outer-loop iteration
  - [`main/voice_onboard.{c,h}`](main/voice_onboard.h) extracted from `voice.c` — owns the entire vmode=4 surface (warmup + per-text failover + autonomous chain)
  - K144's `single_speaker_english_fast` SummerTTS crashes mid-stream in chained `tts.setup`; workaround is per-utterance synth via `voice_m5_llm_tts` posted to `tab5_worker` on every LLM `finish=true`
  - `GET /m5` debug endpoint returns `{chain_active, chain_uptime_ms, failover_state, uart_baud}` — closes the diagnostic gap where "chain didn't start" required serial+adb
  - Obs events: `m5.warmup` (start/ready/unavailable), `m5.chain` (start/stop) feed the existing `/events` ring + e2e harness
  - `tests/e2e/scenarios/runner.py` `story_onboard` (14 steps) covers vmode=4 lifecycle
  - Full retrospective: `LEARNINGS.md` "K144 chain hardening (audit 2026-04-29) — 7-wave program closes 14/18 audit findings"
  - Plan + audit docs: [`docs/AUDIT-k144-chain-2026-04-29.md`](docs/AUDIT-k144-chain-2026-04-29.md), [`docs/PLAN-k144-chain-hardening.md`](docs/PLAN-k144-chain-hardening.md)
- **Live performance:**  ~4.5s boot warm-up (cold-start NPU model load), ~2-3s per turn after warm.  K144 reply renders as a "TINKER" bubble in the chat overlay.
- **Voice modes — five tiers now (was four):**
  - `vmode=0` Local — Dragon Q6A LLM (existing)
  - `vmode=1` Hybrid — Dragon LLM + OpenRouter STT/TTS (existing)
  - `vmode=2` Cloud — OpenRouter LLM + STT + TTS (existing)
  - `vmode=3` TinkerClaw — TinkerClaw Gateway (existing)
  - `vmode=4` Onboard — **K144 LLM, no Dragon needed** (new in Phase 5)
- **Failover behavior:**  In Local mode (`vmode=0`), if Dragon WS is unreachable for ≥30s AND the K144 is warm AND the user sends a text turn, voice.c routes to the K144 automatically.  Toast: "Using onboard LLM".  When Dragon comes back, next text turn returns to Dragon and shows "Dragon reconnected" toast.
- **K144 cold-start guard:**  Boot warm-up posts a probe + one synchronous `voice_m5_llm_infer("hi", ...)` to map the model into NPU memory.  Up to 6 minutes (cold-start budget).  On success the failover gate flips READY; on any failure (probe timeout, NPU hang) it flips UNAVAILABLE.  **TT #328 Wave 13 (`4352e9e`) made UNAVAILABLE recoverable in software** — `voice_onboard_reset_failover()` sends `sys.reset` to the StackFlow daemon, waits for re-init, re-runs the warmup probe.  Auto-retries every 60 s (capped at 3 attempts/boot via `esp_timer`); manual recovery available via tap on the K144 health chip in Settings or `POST /m5/reset` debug endpoint.  Live timing: 9.6 s end-to-end on hardware.  See [`docs/PLAN-k144-recovery.md`](docs/PLAN-k144-recovery.md) for the verified `sys.*` verb surface (probed 2026-05-01) — `sys.reset`, `sys.reboot`, `sys.hwinfo`, `sys.lsmode`, `sys.version` are real; `sys.list`, `sys.status`, `sys.uptime`, `sys.log` are not.  User-facing flows are NEVER blocked by K144 hang behaviors.

### Talking to a stacked K144 from the dev host (debugging)

### Talking to a stacked K144 from the dev host (debugging)

ADB still works the same on the Mate-stacked topology — the **K144's top USB-C** (M140 module port) is what exposes Axera ADB.  Plug your dev-host USB-C into THAT one (not the Mate carrier's USB-C, which is just a power feed for the K144).

```bash
# Module enumerates as Axera ADB (vendor 32c9, product 2003)
lsusb | grep -i axera
# → Bus 001 Device 003: ID 32c9:2003 axera ax620e-adb

# udev rules aren't installed — restart adb as root to claim the device
sudo adb kill-server && sudo adb start-server && sudo adb devices
# → axera-ax620e device

# Open a shell on the AX630C Linux (root, no password)
sudo adb shell
# Useful commands once in:
#   systemctl is-active llm-llm        # confirm StackFlow up
#   uptime                              # confirm cold-boot timestamp
#   apt list --installed | grep llm-    # see installed models / units
#   ss -tln | grep 10001                # confirm StackFlow TCP listener

# Forward the StackFlow JSON service to the host for direct testing
sudo adb forward tcp:10001 tcp:10001
# Then send newline-delimited JSON via nc/python — see PLAN-m5-llm-module.md
# Phase 0 results section for the full request shapes + bench harness.
```

**Wire protocol gotchas (captured in plan doc):**
- Setup returns non-streaming ack with `data:"None"` first; the `work_id` (e.g. `llm.1000`) is the resource id for inference.
- Inference streams shape: `{"object":"llm.utf-8.stream", "data":{"delta":..., "index":N, "finish":bool}}`.
- Both `data:object` and `data:string` accepted on the inference REQUEST.
- Newline-delimited JSON, single TCP socket survives multiple inference calls — no length-prefix framing.

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

### Pre-push format check (CI gates on this)

CI runs `git-clang-format --diff origin/main` against your changed C/H lines (PR #309 / #318).  Pre-existing format violations elsewhere in `main/` are intentionally ignored, but **your diff must be clean**.  Reproduce locally:

```bash
git fetch origin main:refs/remotes/origin/main
git-clang-format --binary clang-format-18 --diff origin/main main/*.c main/*.h
# Empty diff or "clang-format did not modify any files" → green CI
# Any output → run without --diff to autofix:
git-clang-format --binary clang-format-18 origin/main
git add -u && git commit --amend --no-edit && git push -f
```

CI build pins **ESP-IDF v5.5.2** (matches `dependencies.lock`).  Local builds must use the same version (`. /home/rebelforce/esp/esp-idf/export.sh`).

### Wave-style closures + plan docs (workflow patterns from prior sessions)

When the issue tracker accumulates >10 open items, work in **waves**: one wave audits + closes stale issues; the next ships 1-3 PRs.  Audit before scoping — many "open Critical" items in tracker docs turn out to be silently shipped via PRs that referenced the audit ID without ticking the master issue.  `git log --grep "audit X"` is the reliable signal.  Pattern caught us in TB #137 / TB #126 / TT #305 / TT #206 (each had 60-95 % stale findings).

For multi-week features (Grove, K144, widget platform), **write a plan doc first** (`docs/PLAN-*.md`) with: hardware reality, code architecture, phased breakdown with file:line refs, honest unknowns, code anchors.  File a tracking issue that links to the doc as source-of-truth.  Branches die; docs + issues persist.

For **non-obvious cross-stack architectural decisions** (host-test pattern, lv_async_call discipline, codec gating policy, etc.), write an ADR — see [`docs/adr/README.md`](docs/adr/README.md) for the criteria + template.  ADRs are short, immutable once accepted, and live forever so future contributors don't re-litigate decisions that already took real thinking to settle.  Not every PR needs one; one a quarter is healthy.

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
- **Host (current 2026-05-04):** `192.168.70.242` (LAN flips between `192.168.1.x` and `192.168.70.x` — historic IP `192.168.1.91` was valid on the .1.x LAN; verify with `ping radxa-dragon-q6a` or `nmap -p 22,3502,18789 --open <subnet>/24`)
- **Hostname:** `radxa-dragon-q6a` (nmap reverse DNS)
- **User:** radxa (NOT rock — user was migrated)
- **Password:** `<DRAGON_SSH_PASSWORD>` (the value passes via `sshpass -p '<pw>'`; sudo on Dragon is currently passwordless after SSH login)
- **SSH:** `ssh radxa@192.168.70.242  # or whatever the current LAN IP is`
```bash
ssh radxa@192.168.70.242
sudo systemctl status tinkerclaw-voice
sudo journalctl -u tinkerclaw-voice --no-pager -n 50
```

### Dragon Stability
- **Ethernet** — DHCP-assigned via enp1s0 (currently `192.168.70.242`; was `192.168.1.91` on the .1.x LAN)
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
**Note:** Tab5 IP is DHCP-assigned.  Current lease 2026-05-04: `192.168.70.128` (hostname `espressif`); was `192.168.70.128` on the .1.x LAN.  When the lease moves, locate via `nmap -p 8080 --open <subnet>/24` (the dev box itself answers 8080 too — Tab5 is the one whose `GET /info` returns `auth_required: true`).

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
curl -s -H "Authorization: Bearer $TOKEN" -o screen.bmp http://192.168.70.128:8080/screenshot
curl -s http://192.168.70.128:8080/info | python3 -m json.tool   # No auth needed

# Touch
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/touch -d '{"x":360,"y":640,"action":"tap"}'

# Settings (read all NVS settings as JSON)
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.70.128:8080/settings | python3 -m json.tool

# Voice Mode (switch Local/Hybrid/Cloud remotely)
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.70.128:8080/mode?m=0"  # Local
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.70.128:8080/mode?m=1"  # Hybrid (cloud STT+TTS)
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.70.128:8080/mode?m=2&model=anthropic/claude-sonnet-4-20250514"  # Full Cloud
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.70.128:8080/mode?m=3"  # TinkerClaw

# Navigation (force screen change — uses async_navigate dispatcher with screen.navigate obs event)
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.70.128:8080/navigate?screen=settings"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.70.128:8080/navigate?screen=notes"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.70.128:8080/navigate?screen=chat"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.70.128:8080/navigate?screen=camera"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.70.128:8080/navigate?screen=home"

# Camera (capture live frame as BMP)
curl -s -H "Authorization: Bearer $TOKEN" -o frame.bmp http://192.168.70.128:8080/camera

# OTA
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.70.128:8080/ota/check | python3 -m json.tool
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/ota/apply

# Chat (send text to Dragon via voice WS)
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/chat -d '{"text":"What time is it?"}'

# Voice state (connected, state_name, last_llm_text, last_stt_text)
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.70.128:8080/voice | python3 -m json.tool

# Force voice WS reconnect
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/voice/reconnect

# K144 / chain diagnostic snapshot (TT #327 Wave 5)
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.70.128:8080/m5 | python3 -m json.tool
# → {"chain_active":false,"chain_uptime_ms":0,"failover_state":2,
#    "failover_state_name":"ready","uart_baud":115200}

# K144 software reset (TT #328 Wave 13) — sends sys.reset to the StackFlow daemon
# on the AX630C, waits for re-init, re-runs the warm-up probe.  Recovers a
# sticky UNAVAILABLE state in ~10 s without needing to power-cycle the K144 or
# reboot Tab5.  Async — returns immediately + flips state machine to PROBING.
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/m5/reset
# → {"status":"queued","detail":"K144 reset job enqueued — poll GET /m5",
#    "failover_state":2}
# → {"status":"rejected","detail":"Probe already in flight — ..."}  (if already cycling)

# K144 hwinfo cache refresh (TT #328 Wave 14) — forces a fresh sys.hwinfo +
# sys.version round-trip outside the 30 s cache TTL.  Returns the same shape as
# GET /m5 with the cache freshly populated.  GET /m5 itself now also surfaces
# the cached hwinfo block on every call.
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/m5/refresh
# → {..., "hwinfo": {"valid":true,"temp_celsius":39.35,"temp_milli_c":39350,
#                    "cpu_loadavg":0,"mem":27,"cache_age_ms":1125},
#       "version":"v1.3"}

# K144 model registry (TT #328 Wave 15) — surfaces the sys.lsmode response
# with full {mode, primary_cap, language} per installed model.  Cached for
# 5 min (registry doesn't change between K144 reboots); ?force=1 bypasses.
curl -s -H "Authorization: Bearer $TOKEN" "http://192.168.70.128:8080/m5/models?force=1"
# → {"valid":true,"count":11,"models":[
#     {"mode":"qwen2.5-0.5B-prefill-20e","primary_cap":"text_generation",...},
#     {"mode":"sherpa-ncnn-streaming-zipformer-20M-2023-02-17",
#      "primary_cap":"Automatic_Speech_Recognition","language":"English"},
#     {"mode":"sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01",
#      "primary_cap":"Keyword_spotting","language":"English"},
#     {"mode":"yolo11n","primary_cap":"Detection","language":""},
#     ... (11 entries on K144 v1.3)
#   ], "cache_age_s": 1}

# Self-test (no auth needed)
curl -s http://192.168.70.128:8080/selftest | python3 -m json.tool

# ── Harness-friendly endpoints (#293/#294/#296/#297, April 2026) ──
# Current screen + overlay visibility (chat/voice/settings)
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.70.128:8080/screen | python3 -m json.tool
# → {"current":"home","overlays":{"chat":false,"voice":false,"settings":false}}

# Type into the focused LVGL textarea — also accepts ?text=&submit=1 query.
# submit=true dispatches LV_EVENT_READY (same event the Done key fires).
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/input/text \
     -d '{"text":"hello world","submit":true}'

# Long-press at a coordinate (450ms default; 500-5000ms range via duration_ms)
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/touch \
     -d '{"x":360,"y":640,"action":"long_press","duration_ms":1200}'

# Swipe between tile pages (20ms step cadence; 50-3000ms total via duration_ms)
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/touch \
     -d '{"action":"swipe","x1":600,"y1":640,"x2":120,"y2":640,"duration_ms":300}'

# Observability events ring — read everything since uptime_ms=N.
# Kinds: obs, screen.navigate, voice.state, ws.connect, ws.disconnect,
#        chat.llm_done, camera.capture, camera.record_start, camera.record_stop,
#        display.brightness, audio.volume, audio.mic_mute, nvs.
# Polling-only (long-poll was tried + reverted: PANIC under load on
# single-threaded httpd — see LEARNINGS).
curl -s -H "Authorization: Bearer $TOKEN" "http://192.168.70.128:8080/events?since=0" | python3 -m json.tool
```

### Observability events
`tab5_debug_obs_event(kind, detail)` (in `main/debug_obs.{c,h}`) writes to a 32-slot
ring; `GET /events?since=N` returns everything with `ms >= N`.  Each entry has
`{"ms": uptime_ms, "kind": "category.subkind", "detail": "..."}`.

| Kind | Where it fires | Detail |
|------|----------------|--------|
| `obs` | Boot | `"init"` once |
| `screen.navigate` | `POST /navigate` handler | target screen name |
| `voice.state` | `voice_set_state()` on every state transition | `IDLE`/`CONNECTING`/`READY`/`LISTENING`/`PROCESSING`/`SPEAKING`/`RECONNECTING` |
| `ws.connect` / `ws.disconnect` | WS event handler | empty |
| `chat.llm_done` | WS llm_done JSON handler | `llm_ms` value as string |
| `camera.capture` | `capture_btn_cb` after photo saved | absolute SD path |
| `camera.record_start` / `camera.record_stop` | `cb_record_btn` toggle | path + frame/byte counts on stop |
| `display.brightness` | `POST /display/brightness` handler | percentage |
| `audio.volume` / `audio.mic_mute` | `POST /audio` handler | new value |
| `nvs` | `POST /nvs/erase` | `"erase"` |
| `m5.warmup` | `voice_onboard_warmup_job` + `reset_failover_job` | `start` / `ready` / `unavailable` |
| `m5.chain` | `voice_onboard_chain_start` / `_chain_stop` | `start` / `stop` |
| `m5.reset` | `voice_onboard_reset_failover` (Wave 13) | `start` / `ack_ok` / `ack_fail` / `auto_retry` / `recovered` / `fail` |
| `error.k144` | `mark_k144_unavailable` | reason: `probe_fail` / `warmup_fail` / `reset_probe_fail` / `reset_warmup_fail` |

The `kind` buffer is 32 chars; `detail` is 48 chars (silently truncated past those).
Ring is 256 entries, FIFO eviction.

## End-to-End Test Harness

Python-driven scenario runner in [`tests/e2e/`](./tests/e2e/) that drives Tab5
through long user-story flows via the debug HTTP server above.  See
[`tests/e2e/README.md`](./tests/e2e/README.md) for the authoring guide.

```bash
cd ~/projects/TinkerTab
export TAB5_TOKEN=05eed3b13bf62d92cfd8ac424438b9f2

python3 tests/e2e/runner.py story_smoke    # ~2 min  — nav + voice + camera basics
python3 tests/e2e/runner.py story_full     # ~2 min  — all 4 voice modes + photo + REC + cloud chat
python3 tests/e2e/runner.py story_stress   # ~10 min — 6 cycles of mode×screen×chat with heap watchdog
python3 tests/e2e/runner.py story_onboard  # ~30 sec — vmode=4 K144 chain lifecycle (TT #327 Wave 5)
                                            #            (skips if /m5 reports failover_state != READY)
python3 tests/e2e/runner.py all --reboot   # all 3 with a clean reboot first
```

Each run produces a per-run directory under `tests/e2e/runs/` (gitignored)
with:
- `report.json` — machine-readable pass/fail per step + events captured
- `report.md`   — human-readable table with inline screenshots
- `NN_<step>.jpg` — per-step JPEG screenshot

Last validated 2026-04-27: **smoke 14/14, full 24/24, stress 76/77 (single
LLM-timeout flake — Q6A latency, not a regression)**.

### Driver primitives
`Tab5Driver` in [`tests/e2e/driver.py`](./tests/e2e/driver.py) wraps the debug
HTTP API.  Key building blocks:
- `tab5.tap(x, y)` / `long_press(x, y, ms)` / `swipe(x1, y1, x2, y2)`
- `tab5.navigate("camera")` (auto-debounced — 600 ms minimum gap)
- `tab5.screen()` / `tab5.voice_state()` / `tab5.heap()`
- `tab5.chat(text)` / `tab5.input_text(text, submit=False)`
- `tab5.mode(0|1|2|3, model="...")`
- `tab5.camera_frame(save_path)` / `tab5.video_call_start(fps)` / `tab5.call_status()`
- `tab5.screenshot("/path.jpg")`
- `tab5.await_event(kind, timeout_s, detail_match=...)` — polls `/events`
- `tab5.await_voice_state("READY", 60)` — polls `/voice` AND watches events
- `tab5.await_screen("camera", 5)` / `tab5.await_llm_done(180)`

### Bugs the harness has caught
Listed in chronological order from real runs:
1. **Cursor-stealing in events polling** — diagnostic per-step snapshot was
   advancing `_last_event_ms`, so subsequent `await_event` missed events fired
   during prior steps.  Fixed via `events(peek=True)`.  See LEARNINGS in
   TinkerBox repo (#92).
2. **`obs_event_t.kind` 16-char buffer too small** — `camera.record_start`
   silently truncated to `camera.record_s`.  Bumped to 32 (#294 / `debug_obs.c`).
3. **Boot `READY` race** — `await_voice_state` only polled future events;
   boot's `voice.state READY` fires before harness reset its cursor.  Fixed
   to also poll `/voice` current state every ~1 s.
4. **`/events` long-poll attempt → PANIC** — ESP-IDF httpd is single-task,
   `vTaskDelay` blocked all other requests + accumulated heap fragmentation.
   Reverted; harness uses 250 ms polling.

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

### Five-Tier Voice Mode (Onboard tier added in Phase 5)
Settings dropdown: **Local / Hybrid / Full Cloud / TinkerClaw / Onboard (K144)**

| Mode | STT | LLM | TTS | Latency | Cost |
|------|-----|-----|-----|---------|------|
| Local (0) | Moonshine | NPU/Ollama (default: ministral-3:3b, ~67s/turn median) | Piper | 60-90s | Free |
| Hybrid (1) | OpenRouter gpt-audio-mini | Local (unchanged) | OpenRouter gpt-audio-mini | 4-8s | ~$0.02/req |
| Full Cloud (2) | OpenRouter gpt-audio-mini | User-selected (Haiku/Sonnet/GPT-4o) | OpenRouter gpt-audio-mini | 3-6s | $0.03-0.08/req |
| TinkerClaw (3) | Moonshine (or OpenRouter) | TinkerClaw Gateway | Piper (or OpenRouter) | varies | varies |
| Onboard K144 (4) | K144 sherpa-ncnn ASR (chain) or text-only (failover) | M5 K144 stacked LLM (qwen2.5-0.5B over UART) | K144 single_speaker_english_fast (per-utterance synth) | 2-3s | Free |

- **LLM Model Picker:** Settings dropdown: Local NPU / Local Ollama / Claude Haiku / Claude Sonnet / GPT-4o mini (enabled only in Full Cloud mode)
- **Auto-Fallback (cloud STT/TTS):** If cloud STT/TTS fails, Dragon falls back to local for that request + sends `config_update` with `error` field → Tab5 auto-reverts to Local mode
- **K144 failover (Local → Onboard):** When `vmode=0` (Local) and Dragon WS is unreachable for ≥30s with the K144 warm, Tab5 routes the next text turn through `voice_m5_llm_infer` automatically.  Toast: "Using onboard LLM".  When Dragon reconnects, next turn returns to Dragon and shows "Dragon reconnected" toast.
- **K144 always-on (Onboard mode):** When `vmode=4`, EVERY text turn routes to K144 regardless of Dragon WS state.  Dragon stays on local STT/TTS but never sees the LLM turn.
- **NVS:** `voice_mode` (uint8: 0-4), `llm_model` (string: model ID)
- **Protocol:** `{"type":"config_update","voice_mode":0|1|2|3,"llm_model":"anthropic/claude-sonnet-4-20250514"}` — Tab5 always sends Dragon a value in 0..3; mode 4 is Tab5-side-only and Dragon's ACK echo is filtered out by voice.c.

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
  scp build/tinkertab.bin radxa@192.168.70.242:/home/radxa/ota/
  # IMPORTANT: Always include sha256 hash for MitM protection
  SHA=$(sha256sum build/tinkertab.bin | cut -d' ' -f1)
  echo "{\"version\":\"0.7.1\",\"sha256\":\"$SHA\"}" | ssh radxa@192.168.70.242 'cat > /home/radxa/ota/version.json'
  # Tab5 checks hourly or user taps "Check Update" in Settings
  ```

## Camera
- **Sensor:** SC202CS 2MP via MIPI-CSI (1-lane, 576MHz, RAW8)
- **Pipeline:** SC202CS → MIPI CSI → ISP (RAW8→RGB565) → /dev/video0 (V4L2)
- **Driver:** Uses `esp_video` + `esp_cam_sensor` stack from M5Stack (NOT raw CSI API)
- **Resolution:** 1280×720 @ 30fps RGB565
- **Exposure:** Tuned for indoor lighting (SCCB register writes post-init)
- **Debug:** `GET /camera` returns live frame as JPEG
- **CONFIG_CAMERA_SC202CS=y** must be set in sdkconfig (sensor won't compile without it!)
- **Software rotation:** NVS `cam_rot` (0/1/2/3 = 0/90/180/270° CW) is applied to each captured frame before display/upload. Settings dropdown + an in-viewfinder "Rot" button writes the key. Added in #261; live cycle button + PIP rotation in #290.

### Photo capture
Tap the white circular shutter button (center of the bottom control bar). Saved
as `/sdcard/IMG_NNNN.jpg` (4-digit counter resumed from existing files on boot).
Emits `camera.capture` obs event with the saved path. When the camera screen was
launched from the chat overlay (via "Send photo" button), the capture also
auto-uploads to Dragon's `/api/media/upload` and announces a `user_image` WS
event so the chat threads it inline.

### Video recording (#291)
Tap the red-bordered REC pill button (right of the shutter, before Gallery).
Records concatenated motion-JPEG to `/sdcard/VID_NNNN.MJP` at 5 fps,
quality 60, with the same `cam_rot` applied so the recording matches the
viewfinder.  Tap REC again to stop.

- **File format:** `.MJP` (3-char extension because `CONFIG_FATFS_LFN_NONE=1`
  forces 8.3 short names — `.mjpeg` would fail). ffmpeg + VLC sniff the magic
  bytes and play these natively: `ffplay -f mjpeg VID_NNNN.MJP`.
- **Encoder:** Shared with `voice_video.c` (the call-streaming module) via
  `voice_video_encode_rgb565()`.  ESP32-P4 has only one HW JPEG engine —
  attempting to allocate a second crashes with "no memory for jpeg encoder
  rxlink".  Mutex-guarded inside `voice_video.c`.
- **Hard cap:** 1500 frames (5 min × 60 s × 5 fps).  Exceeding the cap auto-
  stops the recording.
- **DMA buffers:** Allocated lazily on first record start via
  `jpeg_alloc_encoder_mem()` (DMA-aligned PSRAM); freed when the camera
  screen is destroyed.  Plain `heap_caps_malloc` doesn't satisfy the JPEG
  engine's bit-stream alignment requirement.
- **Auto-upload to Dragon:** On stop, `voice_upload_chat_image(s_rec_path)`
  POSTs the file to `/api/media/upload`.  The response `media_id` is then
  announced as a `user_image` WS event so it appears in the chat thread.
- **Obs events:** `camera.record_start` (path) and `camera.record_stop`
  (`path frames=N bytes=N`).  The e2e harness (`story_full`) exercises the
  full start → 6 s record → stop cycle.

## Two-way Video Calling (April 2026)

End-to-end Tab5 ↔ Dragon ↔ (Tab5 or web client) video + audio calls. Built across PRs #267 / #269 / #271 / #273 (Tab5 side) and #176 / #178 / #180 / #181 (Dragon side).

- **Module:** `main/voice_video.{c,h}` — uplink encode (HW JPEG via `esp_video`) and downlink decode (TJPGD into LVGL canvas).
- **Wire format:** Both directions use `"VID0"` magic + 4-byte BE length + JPEG bytes (see WebSocket Protocol section). Audio in calls uses `"AUD0"` + 4-byte BE length + raw 16 kHz mono int16 PCM.
- **Atomic helpers:** `voice_video_start_call()` / `voice_video_end_call()` wrap mode flip + camera open/close + UI show/hide so no caller has to coordinate the three subsystems.
- **VOICE_MODE_CALL:** New voice mode that spawns a mic task wrapping frames as AUD0 (NOT untagged PCM into STT). When the call ends, the mode reverts to whatever `vmode` was before.
- **UI:** `main/ui_video_pane.{c,h}` is the full-screen video overlay. In-call it adds a 240×135 local-camera PIP in the corner + a red "End Call" pill. The nav sheet has a "Call" tile that triggers `voice_video_start_call()`.
- **Debug endpoints** (all bearer-auth except where noted):
  - `POST /video/start` / `POST /video/stop` — start/stop uplink JPEG stream to Dragon
  - `POST /video/show` / `POST /video/hide` — toggle the downlink video pane (without starting a call)
  - `POST /call/start` / `POST /call/end` — atomic call begin/end (uses voice_video_start_call/end_call)
  - `GET /video` — video stats (frames sent/received, last frame size, pane state)

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
- **OPUS encoding:** Capability-negotiation infrastructure shipped in #263/#265 (Tab5) + TinkerBox #174. **Encoder is gated OFF in `voice_codec.h` pending issue #264** — SILK NSQ crashes on ESP32-P4 mid-encode; root-cause TBD. Decoder is ready for Phase 2B Dragon→Tab5 OPUS TTS.

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
| `vmode` | u8 | 0 | 0-4 | Voice mode: 0=local, 1=hybrid, 2=cloud, 3=TinkerClaw, 4=onboard (K144) |
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
| `cam_rot` | u8 | 0 | 0-3 | Camera frame rotation in 90° steps applied in software after capture (0=none, 1=90° CW, 2=180°, 3=270° CW). Settings dropdown. Added in #261. |
| `dragon_tok` | str | `""` | — | Dragon REST API bearer token.  Used for outbound HTTP calls to gated endpoints (`/api/v1/tools`, `/api/v1/agent_log`, `/api/v1/sessions`, `/api/v1/memory`).  Empty by default; provisioned via `POST /settings`.  Wave 8 (`9f51804`). |
| `star_skills` | str | `""` | — | Comma-separated list of starred (pinned) skill / tool names.  Read by `ui_skills.c` on render to sort starred tools first + apply amber tint + "PINNED" caption.  Toggled by tap on a skill card or via `POST /settings`.  Wave 11 (`bcf05d9`). |

### ⚠️ LVGL Configuration — CRITICAL
**ALL LVGL config goes in `sdkconfig.defaults`, NOT `lv_conf.h`.** The ESP-IDF LVGL component sets `CONFIG_LV_CONF_SKIP=1` which means `lv_conf.h` is COMPLETELY IGNORED. Any change to `lv_conf.h` has ZERO effect. Always verify with `grep "SETTING" build/config/sdkconfig.h` after building.

Current LVGL settings in `sdkconfig.defaults`:
- **Memory pool:** `CONFIG_LV_MEM_SIZE_KILOBYTES=64` is the BSS-allocated base pool (was 96; dropped to 64 in #185 to free internal-SRAM headroom — see "Key Fixes" below for the full root-cause).  Soft ceiling for this firmware layout; 128+ aborts at boot in `main_task` per Phase 3-A.  `CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=4096` is **only a TLSF per-pool max-size ceiling** (`TLSF_MAX_POOL_SIZE = LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE` at `lv_tlsf.c:13`) — **NOT** an auto-expand-on-demand trigger.  LVGL 9.2.2 has no auto-expand; the only way to grow the heap is to call `lv_mem_add_pool()` ourselves.  `main.c` does exactly that at boot with a 2 MB PSRAM-backed chunk, giving ~2048 + 64 KB of working heap.  Verified under stress: `free_size` stabilises with generous headroom; zero LVGL PANIC crashes.  See `docs/STABILITY-INVESTIGATION.md`.
- **Circle cache:** `CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE=32`. Default 4 causes crashes when 5+ rounded objects render simultaneously.
- **Asserts disabled:** `CONFIG_LV_USE_ASSERT_MALLOC=n` and `CONFIG_LV_USE_ASSERT_NULL=n` — prevents `while(1)` hang on alloc failure (which triggers 60s WDT reboot). With asserts off, NULL propagates and crashes faster with a useful backtrace instead of a silent WDT hang.
- **JPEG decode:** `CONFIG_LV_USE_TJPGD=y` + `CONFIG_LV_USE_FS_MEMFS=y` — enables TJPGD decoder for in-memory JPEG rendering (used by rich media chat).
- **Render mode:** `LV_DISPLAY_RENDER_MODE_PARTIAL` with two 144KB draw buffers in PSRAM. Do NOT use DIRECT mode (causes tearing on DPI).

### Key fixes — see CHANGELOG

The 24-bullet "Key Fixes (April 2026)" list (LVGL pool OOM, circle cache,
settings WDT, mode-aware timeouts, voice overlay instant hide, debug-
server bearer auth, etc.) has moved to [`docs/CHANGELOG.md`](docs/CHANGELOG.md)
so the runbook stays context-light.  The principles (hide/show overlay
pattern, fragmentation watchdog) remain below.

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

### Binary frame magic tags (added April 2026)
Tab5↔Dragon binary frames are now disambiguated by an 8-byte prefix on non-audio payloads. **Untagged binary frames remain raw 16 kHz PCM and are routed straight into the STT pipeline** — that's the legacy mic path and it MUST stay magic-less for backward compat. Tagged frames carry one of:

| Magic | Direction | Length field | Payload | Module |
|-------|-----------|--------------|---------|--------|
| `VID0` | both | 4 bytes BE u32 | JPEG frame | `voice_video.{c,h}` — Tab5 camera → Dragon (uplink) and Dragon → Tab5 (downlink playback) |
| `AUD0` | both | 4 bytes BE u32 | raw 16 kHz mono int16 PCM | `voice.c` call mode — VOICE_MODE_CALL bypasses STT, mic frames are wrapped with AUD0 instead of being sent untagged |

Wire layout: `"VID0"` (4 bytes) + `len_be` (4 bytes) + `payload[len]`. Same shape for `"AUD0"`. Frames without the 4-byte magic are still treated as raw PCM going to STT.

### Tab5 → Dragon (sending)
1. **Voice Input:** `{"type":"start"}` (with optional `"mode":"dictate"`) -> binary PCM frames (no magic) -> `{"type":"stop"}`
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
main/debug_server.{c,h}   — HTTP debug server core: httpd lifecycle, bearer-token
                             auth (init_auth_token + tab5_debug_check_auth), shared
                             send_json_resp helper, /info /index /selftest, plus the
                             18 family register calls.  Wave 23b (#332) thinned this
                             from 4,520 LOC → 849 LOC (-81.2%) by extracting per-
                             family modules.  Endpoint count:
                               grep -c 'httpd_register_uri_handler' main/debug_server*.c
main/debug_server_admin.c     — /reboot, /sdcard, /widget injection
main/debug_server_call.c      — /video/* + /call/* (12 video-call endpoints)
main/debug_server_camera.c    — /screenshot, /screenshot.jpg, /camera + JPEG encoder
main/debug_server_chat.c      — /chat, /chat/messages, /chat/llm_done, /chat/partial,
                                 /chat/audio_clip, /voice/text alias
main/debug_server_codec.c     — /codec/opus_test
main/debug_server_dictation.c — /dictation (POST + GET)
main/debug_server_inject.c    — /debug/inject_audio, _error, _ws (test-harness only)
main/debug_server_input.c     — /touch, /input/text + LVGL touch-injection seqlock
                                 (tab5_debug_touch_override is the LVGL touch_read_cb
                                 reader)
main/debug_server_m5.c        — /m5, /m5/reset, /m5/refresh, /m5/models (K144)
main/debug_server_metrics.c   — /tasks, /logs/tail, /battery, /display/brightness,
                                 /audio (GET+POST), /metrics (Prometheus), /events,
                                 /heap/history, /heap/probe-csv, /keyboard/layout,
                                 /tool_log/push, /net/ping
main/debug_server_mode.c      — /mode (voice mode + LLM model picker)
main/debug_server_nav.c       — /navigate, /screen + tab5_debug_set_nav_target
                                 (called from ui_chrome / ui_chat / ui_camera /
                                 ui_files / ui_notes / ui_settings / ui_wifi on
                                 swipe-back / back-button / home-button paths)
main/debug_server_obs.c       — /log, /crashlog, /coredump, /heap_trace_*, /heap
main/debug_server_ota.c       — /ota/check, /ota/apply
main/debug_server_settings.c  — /settings (GET+POST), /nvs/erase
main/debug_server_voice.c     — /voice (state), /voice/reconnect, /voice/cancel,
                                 /voice/clear
main/debug_server_wifi.c      — /wifi/kick, /wifi/status
main/debug_server_internal.h  — shared check_auth + send_json_resp helpers used by
                                 every family module
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
main/voice.{c,h}          — Voice public API + state machine + mic capture task +
                             playback drain task + listening/dictation/call lifecycles +
                             reconnect watchdog.  Wave 23 thinned voice.c from ~3,668 LOC
                             to ~2,287 LOC by extracting voice_ws_proto (RX/TX dispatch +
                             event handler) and voice_modes (five-tier routing).
main/voice_ws_proto.{c,h} — WS frame routing layer: JSON RX dispatcher + binary magic
                             VID0/AUD0/untagged-PCM dispatcher + esp_websocket_client
                             event callback + send wrappers + REGISTER frame builder +
                             UI-async helpers (toast / banner / badge dispatchers) +
                             eviction/auth-fail stop workers.  Wave 23 SRP closure for
                             TT #331-A (PR #355).
main/voice_modes.{c,h}    — Five-tier voice-mode dispatcher: voice_send_config_update*
                             senders + voice_modes_route_text decision (LOCAL / HYBRID /
                             CLOUD / TINKERCLAW / LOCAL_ONBOARD) + s_voice_mode
                             ownership.  Wave 23 SRP closure for TT #331-B (PR #356).
main/voice_video.{c,h}    — Two-way video call module: HW JPEG uplink + TJPGD downlink
                             decode, VID0 framing, voice_video_start_call/end_call atomics.
main/voice_codec.{c,h}    — OPUS capability negotiation (decoder ready, encoder gated off
                             pending #264 SILK NSQ crash on ESP32-P4).
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
main/ui_video_pane.{c,h}  — Full-screen downlink-video overlay; in-call adds a 240×135
                             local-camera PIP + red End-Call pill.
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
| Home | ui_home.c | v4·C Ambient Canvas — single screen with orb, mode pill, status strip, say-pill, nav-sheet menu chip (no tileview; the 4-page tileview was retired in v4·C) |
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

## Sprint history + wave program log

Phase 1 / Phase 2 status, dated wave-by-wave notes (W11–W19 K144 +
camera + widget icons + OPUS), and queued candidates (KWS revival,
Grove, external-hardware push) have moved to
[`docs/CHANGELOG.md`](docs/CHANGELOG.md) to keep this runbook
context-light.  For the active 2026-05-11 cross-stack audit + wave
program, see [`docs/AUDIT-state-of-stack-2026-05-11.md`](docs/AUDIT-state-of-stack-2026-05-11.md).

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
curl -sS "http://192.168.70.242:3502/api/ota/check?current=0.0.0" \
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

