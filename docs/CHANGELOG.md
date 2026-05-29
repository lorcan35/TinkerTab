# TinkerTab Changelog

Sprint history + wave program log for the Tab5 firmware.

`CLAUDE.md` is the runbook (loaded into every conversation context) and
intentionally stays compact.  Anything dated, post-mortem, or
sprint-shaped lives here.  For active investigations + open work,
see the "Active Investigations" section at the top of `CLAUDE.md`.

For ESP-IDF + LVGL gotchas + non-obvious root causes, see
[`LEARNINGS.md`](../LEARNINGS.md).  For audit-driven wave programs
that span both repos (TinkerTab + TinkerBox), see
[`docs/internal/AUDIT-state-of-stack-2026-05-11.md`](internal/AUDIT-state-of-stack-2026-05-11.md).

---

## 2026-05-25 — Voice loop + WS health audit (6 PRs)

Live cross-stack audit of Tab5↔Dragon↔K144 piping, triggered by the user-reported "X-button-cancel re-opens listening" loop.  Six independent bugs closed:

- **#693** (closes #692) — Wave A: revert PR #691's permanent wakeword-disarm; `voice_wakeword_post_cancel_suppress_ms(3000)` API installed by `voice_cancel`; ext_pcm pump pause 1500→3000 ms; wake matcher now anchors on freshly-arrived ASR delta (first-half + 4 char slack) instead of accumulated 96 B sliding window.  Fixes "X stops the turn but next ASR partial re-fires wake" loop without killing always-on listener.
- **#699** (closes #698) — Drop consecutive identical non-finish ASR partials at front of `asr_partial_cb`.  K144 sherpa-ncnn rolling decoder re-emits same partial 10×/sec — every duplicate was running 7× istrstr matcher scans + transcript ring critical section + 2× `ESP_LOGI` for zero new signal.  Also demoted `voice_m5_llm.c` asr-frame + asr-delta `ESP_LOGI` to `ESP_LOGD` (was bring-up debug, never cleaned up).
- **#697** (closes #696) — WS-RX transient error branch in `voice_ws_proto.c` now snaps `voice_state` to READY/IDLE.  Pre-fix a `stt_empty`/`pipeline_failed` left Tab5 stuck in PROCESSING forever; next mic tap + wake refused until reboot.  Fatal branch already did the snap-back; transient was missing parity.
- **#695** (closes #694) — Top-right reboot button on home, just left of the mute toggle.  Tap = "Hold to reboot" safety toast, long-press = "Rebooting…" + `esp_restart()` after 600 ms esp_timer one-shot so the toast paints before LVGL gets cut.
- **#701** (closes #700) — `vision_service::detections_total` counts FILTERED (person-class) detections only.  Was counting raw YOLO output (chairs/cups/books) while `last_class` etc. only updated for the filter, making `/vision/state` show `detections_total=3 + last_class=""`.
- **#703** (closes #702) — `ui_notes_unprocessed_count()` drops the FAILED-with-audio branch.  Counter was counting notes that `transcription_queue_task` deliberately refuses to retry (manual-only via UI Retry button to avoid infinite loops on broken audio), making `/logs/tail` log "Transcription queue: 1 unprocessed" every 15 s forever.

**Audit also confirmed clean** (no fix needed): `voice_set_state` mutex-guarded, settings NVS mutex-guarded, cJSON `Parse`/`Delete` balance in `voice_ws_proto.c` (all 4 early returns paired), playback ring buffer mutex-guarded.  `WS_CLIENT_PONG_SEC=180` deliberately long (Dragon aiohttp keepalive runs on its own loop — see voice.c:134 comment).

**Live verification:** post-flash @192.168.1.90 — state=READY, wakeword armed, asr.1005 binding clean, 0 panic/abort/leak/oom log lines, internal-SRAM `largest=62 KB @ 60s` (UP from 52 KB @ 22 min pre-fix — dedup + Wave A reduced steady-state heap pressure).

---

## Phase 1 — Voice Assistant + UI (April 2026)

Feature-complete:

- Ask mode (PTT voice → STT → LLM → TTS) — working end-to-end
- Dictation mode (long-press, auto-stop, adaptive VAD, 64 KB buffer, post-processing) — working
- Text input via chat screen (`voice_send_text`) — working
- Cloud mode toggle (local ↔ OpenRouter STT + TTS) — working
- Device registration + session resume over WebSocket — working
- 7 UI screens + keyboard overlay + voice overlay — all functional
- NVS settings persistence (Wi-Fi, Dragon, volume, brightness, cloud mode, session) — working

## Phase 2 — AEC + Wake Word (RETIRED in #162)

ESP-SR v2.4.0, AFE pipeline, `/wake` debug endpoint, Settings toggle, and the
3 MB model partition were all wired up, but the TDM slot mapping for the AEC
reference channel never produced usable wake-word detection. The feature was
parked for months and then removed in #162 so the ~450 LoC + `esp-sr`
component + partition weren't a permanent maintenance drag.

**Revival path:** restore the `afe.{c,h}` + voice.c/voice.h wake-word stubs
from git history (pre-#162), re-add `esp-sr` to `CMakeLists.txt` +
`idf_component.yml`, and procure a custom "Hey Tinker" WakeNet model + tone-
test the TDM slots to resolve the original blocker.

## Phase 2 — Remaining priorities

1. Desktop SDL2 simulator for fast dev loop (TT #38)
2. OPUS audio encoding (TT #262 — encoder gated off; SILK NSQ crash on ESP32-P4 → TT #264) **— closed in Wave 19**
3. ~~OTA firmware updates~~ — DONE; production has SHA256-verified OTA + auto-rollback (see CLAUDE.md "OTA Firmware Updates")

## Late-April 2026 sprint — shipped during waves 1–10

- Spring animation engine + 3 wirings (toast slide-in / orb size transitions / mode-dot pulse) — `main/spring_anim.{c,h}` (closes #42)
- Audio playback at 1.5× speed bug — `UPSAMPLE_BUF_CAPACITY` was halving the upsample buffer's high-half capacity, dropping 33 % of every TTS chunk.  Fix: bumped from 8192 → 16384 samples (LEARNINGS entry covers the symptom-class)
- UX honesty audit — killed hardcoded "EARLIER TODAY" demo strings in `ui_focus.c`, gated now-card → Agents nav on `tool_log_count() > 0` (closes 23/24 of #206 audit)
- mypy strict on a curated clean list — `dragon_voice/api/{__init__,utils,system}.py` (TinkerBox PR #199)
- Multi-model router + 35 OpenRouter models (TinkerBox #185-#188)

## Early-May 2026 — waves 11–19 shipped

- **Wave 11** (`bcf05d9`) — Skill starring/pinning in `ui_skills.c`.  New NVS key `star_skills` (comma-separated tool names); tap-to-toggle on each catalog card; starred tools sort to the top with amber tint + "PINNED" caption.  PSRAM-allocated kept_payload (NOT BSS — see LEARNINGS "BSS-static caches >3 KB push Tab5 over a boot SRAM threshold").  24/24 e2e steps pass.
- **Wave 12** (`67b9989` Tab5 + `d9a18e4` Dragon) — Cross-session agent activity feed.  Dragon side: new `/api/v1/agent_log` REST endpoint backed by a 64-slot ring populated at the `ToolRegistry.execute` chokepoint (captures WS conversations + REST + dashboard tools uniformly).  Tab5 side: `ui_agents` fetches the feed on every overlay show and renders it below the local empty-state when `tool_log_count() == 0`.  17/17 e2e + 13/13 pytest pass.
- **Wave 13** (`4352e9e`) — K144 is recoverable.  Closes audit gap "UNAVAILABLE state is sticky" — pre-Wave-13 a single failed warmup probe required Tab5 reboot to escape.  Implementation: `voice_m5_llm_sys_reset()` + `voice_onboard_reset_failover()` + `POST /m5/reset` debug endpoint + `esp_timer` 60s auto-retry (capped at 3 attempts/boot, NOT FreeRTOS xTimer per the LEARNINGS entry on that class of failure) + tap-to-recover on the K144 health chip in Settings.  Live timing: 9.6 s reset round-trip on hardware.  17/17 e2e pass.  See `docs/internal/PLAN-k144-recovery.md`.
- **Wave 14** (`fcb5d1e`) — K144 is observable.  `voice_m5_llm_sys_hwinfo()` + `voice_m5_llm_sys_version()` typed wrappers; `GET /m5` enriched with `hwinfo` block (temp_celsius, cpu_loadavg, mem, cache_age_ms) + top-level `version` field; new `POST /m5/refresh` forces fresh fetch outside the 30 s TTL.  Settings UI gauge below the K144 chip shows live `NPU 38.4°C · load 0 · v1.3`.  Two-tier caching (30 s success TTL + 5 s attempt rate-limit) avoids UART hammering under poll spam.  12/12 e2e pass.
- **Wave 15** (`bb2b284`) — K144 model registry surfaced.  `voice_m5_llm_sys_lsmode()` + `GET /m5/models` (PSRAM-cached 5 min; `?force=1` bypasses).  Settings UI inventory line below the gauge: "11 MODELS · 1 LLM · 2 ASR · 3 TTS · 2 KWS · 3 vision" — compact bucket summary with zero-categories elided.  Picker UI deferred (only 1 LLM today; data path ready for when M5 ships a 2nd).  12/12 e2e pass.
- **Wave 16** (`7bacf5c`) — K144 polish.  Closes two paper cuts: (a) Settings stale-state — chip + gauge + inventory now re-render live on every state transition via `refresh_k144_chip()` hooked into `ui_settings_update()`; (b) auto-retry banner now clears on recovery via new `mark_k144_recovered()` helper that resets the retry budget, cancels the pending esp_timer, and calls `ui_home_clear_error_banner()`.  Both wired into the boot warmup + Wave 13 reset paths.  13/13 e2e pass.
- **Wave 17** (`13c2acd`) — Camera lifecycle hybrid (closes TT #247).  `ui_camera_destroy` now does `lv_obj_clean(scr_camera)` instead of `lv_obj_delete` + keeps `canvas_buf` resident; `alloc_canvas_buffer` is idempotent (memset on dimension match, free+realloc on change); `ui_camera_create` fast-path checks `lv_obj_get_child_cnt`.  Live-verified: 397 s mixed-screen stress with no reboots (pre-fix: reboot every 90-120 s).  New LEARNINGS entry "lv_obj_clean is the third option."
- **Wave 18** (`c6fc98a`) — Widget icon library (closes TT #69).  16 built-in glyphs (clock/briefcase/laundry/coffee/book/car/pot/person/droplet/check/alert/sun/moon/cloud/calendar/star) encoded as path-step DSL in `main/widget_icons.{c,h}`; rendered via single `lv_canvas` widget per icon with ARGB8888 PSRAM buffer; tone-driven color (calm→emerald / active→amber / urgent→rose).  Slot rendered top-right of live card; live-verified with clock+active, check+success, alert+urgent.
- **Wave 19** (`d3cc647`) — OPUS encoder stack overflow root-caused (closes TT #264 + #262).  PR #263 had gated the encoder OFF after panics inside SILK NSQ functions; original investigator bumped voice_mic stack 8 → 16 KB and concluded "not stack overflow."  Wave 19 bisected via a new `/codec/opus_test` synthetic endpoint and found 24 KB is the actual watermark — bumped `MIC_TASK_STACK_SIZE` 16 → 32 KB (PSRAM-backed, free).  esp_audio_codec also bumped 2.4.1 → 2.5.0.  Live: 200 frames encoded clean, ~24 B/frame, 26× compression vs PCM.  Unblocks Phase 2B Tab5→Dragon OPUS uplink.

## Mid-May 2026 — orb arc + dictation + wakeword

- **PR [#569](https://github.com/lorcan35/TinkerTab/pull/569)** (`678cc06`, 2026-05-15) — Cubic-Hermite TTS upsample replaces linear interp.  Fixes choppy Kokoro-class TTS on Tab5's 16 → 48 kHz playback path.  4-sample Catmull-Rom kernel + chunk-boundary context cache in `audio.c`; same CPU class as linear, imperceptible cost.  Companion bugfix `0bc87bb` corrects the off-by-one in the upsampler.
- **PR [#573](https://github.com/lorcan35/TinkerTab/pull/573)** (`02bd802`, 2026-05-15) — Dictation cap bumped 5 min → 4 h for meeting-length recording.  `MAX_RECORD_FRAMES_DICT` in `voice.c` goes from 15 000 to 720 000 frames; PSRAM scratch buffer + WS drain handle the new ceiling without I/O changes.  Aligns with the wakeword dictation cap (PR #576).
- **PR [#574](https://github.com/lorcan35/TinkerTab/pull/574)** (`e3aaba9`, 2026-05-16) — Four mic-driven sphere additions to IDLE orb: rim halo (RMS), lit-from-within (LPF energy), specular wobble (transients), and frequency-band hue tint (3-band FFT).  Each toggle-able in the orb config struct; adds orthogonal information dimensions to the existing ambient sphere from PRs #547–#562.
- **TT [#575](https://github.com/lorcan35/TinkerTab/issues/575)** (2026-05-17) — Tracking issue: "Always-on ASR wakeword + on-device dictation via K144."  Frames the problem (TT #162 retired wake-word due to TDM-AEC blocker; K144 now provides a working streaming Zipformer ASR + KWS dead-end documented) + the design.
- **PR [#576](https://github.com/lorcan35/TinkerTab/pull/576)** (`83f82e3` + `e5426cc`, 2026-05-17) — Always-on K144 ASR wakeword + on-device dictation.  New `main/voice_wakeword.{c,h}` (state machine + 32 KB PSRAM dictation buffer + force-stop API) + `voice_m5_llm_wakeword_setup/_run/_teardown` chain helpers + lifecycle hooks in `voice_onboard.c` (warmup READY + Wave 13 reset).  UI bridge in commit `e5426cc` adds toast + orb ripple via `tab5_lv_async_call` on WAKE / DICTATION_FINAL.  Live-verified on Tab5 192.168.1.90 (wake fired on "tinker"); UI bridge needs hardware retest.  See [`docs/internal/PLAN-wakeword.md`](internal/PLAN-wakeword.md).

## Cross-stack waves (May 2026) — see audit doc

For waves W1–W9 of the 2026-05-11 cross-stack audit (SOLO mode, turn_id,
cost guard, mode-3 sub-program, UX polish, test infra),
see [`AUDIT-state-of-stack-2026-05-11.md`](internal/AUDIT-state-of-stack-2026-05-11.md).

## Queued sprints

### Wave 20+ candidates

- **KWS revival on K144** — sherpa-onnx-kws-zipformer-gigaspeech is open-vocabulary (pass keyword list at runtime, no custom training).  **Update 2026-05-17:** PR #576 attempted the direct `kws.setup` path and found the K144 daemon's parser rejects every body shape (`parse_config false`); pivoted to ASR-based phrase matching which gives the same open-vocab guarantee + a free transcript.  Native KWS path remains queued for when M5 fixes the parser upstream — see LEARNINGS "K144 KWS unit is officially installed but unusable."

### External-hardware push parked

- Grove sensor support (TT #316 / `docs/internal/PLAN-grove.md`)
- M5 LLM Module integration (TT #317 / `docs/internal/PLAN-m5-llm-module.md`) — Phase 0 done; phases 1–4 ready

---

## Key fixes shipped during April 2026

(Pre-Wave-1; extracted from CLAUDE.md during W10-A.  Each bullet captures
the symptom + fix; for full root-cause investigations see
[`LEARNINGS.md`](../LEARNINGS.md) and
[`docs/STABILITY-INVESTIGATION.md`](STABILITY-INVESTIGATION.md).)

- **LVGL pool OOM crash fixed:** Notes edit overlay exhausted the (then-96 KB) base pool → `lv_malloc` NULL → NULL-deref inside LVGL draw pipeline → PANIC reboot (cadence ~170 s under stress). Full root-cause investigation in `docs/STABILITY-INVESTIGATION.md` across 12 whack-a-mole symptom PRs + Phase 1-3. Real fix shipped in three PRs (2026-04-24): #183 calls `lv_mem_add_pool()` at boot with a 2 MB PSRAM chunk in `main.c` (LVGL 9.2.2 has no auto-expand — `CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES` is only a TLSF per-pool max-size ceiling); #184 adds an SRAM exhaustion detector to `heap_watchdog.c` so any future drift produces a clean PANIC + coredump rather than a silent SW reset; #185 drops the BSS base pool from 96→64 KB to free internal-SRAM headroom (96 was the soft link-time ceiling, 64 is well below it and the 2 MB PSRAM pool carries the real load).
- **Circle cache crash fixed:** 4 cache entries overflowed with 7+ rounded cards → `circ_calc_aa4` NULL dereference. Fix: `CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE=32` in `sdkconfig.defaults`.
- **Settings WDT crash fixed:** `f_getfree()` on a 128 GB SD card blocks the LVGL thread for ~30 s, triggering the watchdog. Fix: cache the `f_getfree` result from boot, feed `esp_task_wdt_reset()` between settings UI sections during creation.
- **Response timeout (mode-aware):** Local mode = 5 min timeout for tool-calling chains (small models are slow). Cloud mode = 1 min timeout. Timeout is mode-aware, set per voice mode.
- **Tool parser tolerant of small model quirks:** qwen3:1.7b adds stray `>` after `</args>`, sometimes omits closing tags. Parser uses tolerant regex with fallback patterns to handle these gracefully.  **2026-04-24 update:** Default Local model swapped to `ministral-3:3b` after an 11-model gauntlet — 0.6b's malformed-XML leaks and silent fails beat the tolerant parser; 4b-class models blew past Tab5's ~30 s WS keepalive and got P13-evicted mid-stream.  See TinkerBox `CLAUDE.md` "Local LLM Benchmarks" + `docs/AUDIT.md` "Local-mode model gauntlet 2026-04-24".
- **Speaker buzzing fixed:** IO expander P1 (SPK_EN) now initialized LOW at boot to prevent speaker buzzing on startup.
- **Settings rewrite:** Replaced with fullscreen overlay using manual Y positioning. No flex layout, no separate screen. Eliminates WDT crash + draw buffer exhaustion.
- **Settings two-pass creation:** Settings overlay uses a two-pass pattern — first pass creates the container, second pass populates sections with `esp_task_wdt_reset()` fed between each. Touch input blocked during creation to prevent partial-UI taps.
- **Wi-Fi:** Switched to DHCP, WPA2-PSK router, lowered auth threshold.
- **Voice WS:** Added TLS cert bundle for ngrok, fixed TCP/SSL transport leaks.
- **Chat UI overhaul:** Live status bar (Ready/Processing/Speaking), tappable mode badge to cycle Local/Hybrid/Cloud, New Chat button, thinking + tool indicator bubbles during LLM processing.
- **Touch feedback system:** `ui_feedback.h/c` module with pressed states on 30+ interactive elements — buttons darken, cards lighten border, icons dim, nav items brighten. 100 ms ease-out transitions.
- **Nav debounce 300 ms:** Prevents rapid-tap crashes from animation race conditions (dismiss + create overlapping).
- **Nav bar on `lv_layer_top`:** Nav bar is rendered on `lv_layer_top()` so it remains accessible and visible regardless of which screen/overlay is active. Not inside the tileview.
- **TinkerClaw voice mode 3:** Added `VOICE_MODE_TINKERCLAW=3` — routes LLM through TinkerClaw Gateway while STT/TTS use Moonshine/Piper locally or OpenRouter as fallback.
- **Voice overlay instant hide:** `ui_voice_hide()` is instant — no fade animation. The 150 ms fade-out caused three bugs: (1) dangling `s_auto_hide` timer pointer (local static with `auto_delete=true` → use-after-free on next READY entry), (2) `fade_done_hide_cb` firing during navigation state changes, (3) `orb_speak_click_cb` stacking on SPEAKING re-entry. All three fixed April 2026.
- **Voice overlay shows on orb tap:** Tapping the orb on the home screen opens the voice overlay and starts listening immediately.
- **Bearer token auth on debug server:** All endpoints except `/info` and `/selftest` require a Bearer token. Token auto-generated on first boot via `esp_random()` and stored in NVS key `auth_tok`.
- **Keyboard text visible while typing:** Text input field stays visible above the keyboard during typing (was previously hidden behind the keyboard).
- **Done key auto-submits:** Done/Enter key on the keyboard dispatches `LV_EVENT_READY` instead of inserting a newline, triggering form submission.
- **Internal SRAM fragmentation monitoring:** Periodic heap check monitors largest free internal SRAM block (not just total free). If largest block stays below threshold for 3 min sustained, triggers a controlled reboot to defragment.
- **LVGL FPS counter:** LVGL flush rate, surfaced as `lvgl_fps` in `GET /info`, for monitoring UI rendering performance.
- **Rich Media Chat:** Chat screen renders inline rich media — syntax-highlighted code blocks, cards, and audio clips as JPEG images. Dragon renders content server-side (Pygments for code, Pillow for tables), Tab5 downloads and displays via LVGL's TJPGD decoder. 5-slot PSRAM LRU cache (~2.9 MB) with zero alloc/free fragmentation. Max 3 media items per LLM response. Downloads yield every 2 ms to not stall voice WS.
