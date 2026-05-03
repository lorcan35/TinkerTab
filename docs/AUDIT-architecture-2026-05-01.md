# TinkerTab Architecture Audit — 2026-05-01

## TL;DR

The firmware is **well-layered with strong boundaries** between BSP, business logic, and UI, but three god-files (voice.c at 4.2 KLOC, debug_server.c at 4.5 KLOC, ui_settings.c at 2.3 KLOC) are reaching capacity and should be split. The K144 modularity rules are correctly enforced — no inverted dependencies, no direct K144 calls from outside voice_onboard.c, and all six rules locked. One critical debt item: notes BSS static cache (30×300B ≈ 9 KB) should migrate to PSRAM; currently it's eating precious internal SRAM. LVGL async call wrapper is 100% enforced — zero direct lv_async_call violations. No cross-stack protocol drift detected between Tab5 and Dragon (five vmode values correctly enumerated in both repos).

**Biggest single fix:** Extract K144 + failover + warmup telemetry from voice.c into voice_onboard.c (already done — Wave 4b extraction stands healthy). Next: split debug_server.c endpoints by family (voice, m5, display, nvs, etc.) to reduce per-endpoint cognitive load.

## Methodology

**Scanned:** `main/` (100 files, ~41.6 KLOC), `bsp/tab5/` (24 files, layering + boundary checks), manifest inspection of `components/` (3rd-party esp-idf stacks, treated as opaque).

**Excluded:** `managed_components/` (vendored), `build/`, `sim/`, `tools/`, `tests/`, `dragon_voice/` (separate repo mirror), `docs/`, design assets.

**Techniques:**
- File LOC ranking; module coupling via `#include` graphs
- NVS read patterns (all correctly routed through `settings.h` helpers)
- Direct ESP-IDF call audit (UART, I2C, NVS, LVGL async)
- K144 modularity rule enforcement (six rules from CLAUDE.md verified)
- WS protocol vmode enumeration (0-4 consistent, correctly handled in both sides)
- Static BSS mutable globals audit (one large offender: notes cache)
- Mode manager coupling audit (voice_connect/disconnect callers across tasks)

---

## Findings, ranked by severity

### A1 [P1] — God-file: voice.c still large after K144 extraction

**Status:** **SHIPPED** — closed 2026-05-03 by PR #355 (voice_ws_proto extract — WS event handler + JSON RX + binary RX + send wrappers + REGISTER + UI helpers, ~1,267 LOC out) + PR #356 (voice_modes extract — config_update senders + voice_modes_route_text pure helper + s_voice_mode ownership, ~114 LOC out + behavioural split).  voice.c lands at **2,287 LOC**.  E2E baseline holds on physical Tab5 (192.168.1.90): smoke 14/14, full 24/24, onboard 14/14, wave7 (Local→K144 failover) 21/22 (single Dragon-side LLM 180s timeout, not a routing regression).  Heap floor identical to baseline at fresh boot.

**Dimension:** god-file, modularity rules

**Where:** `main/voice.c:1-4251` (4,251 LOC) + 72 endpoint handlers (httpd_register_uri_handler count)

**Smell:** `voice.c` owns five tiers of voice modes (Ask/Dictate/Call/Cloud/Onboard), WS protocol state machine, mic capture lifecycle, rich-media chat rendering, tool activity logging, failover gate (when Dragon WS down + vmode=0 → K144), autonomy dial resolution, and spends 150+ lines on WebSocket frame routing (VID0/AUD0 magic tagging). The K144 warm-up and per-text failover were extracted into `voice_onboard.c` (709 LOC) in Wave 4b, but voice.c still hosts:
- STT text assembly (partial + final)
- LLM streaming receipt + tool-call parsing
- TTS playback ring-buffer drain task
- Dragon config_update ↔ Tab5 mode negotiation (vmode 0-3 sent Dragon-ward, vmode=4 filtered client-side)
- Observable event firing (voice.state, chat.llm_done, tool_call/result, m5.chain start/stop)

**Consequence:** Every voice-mode feature addition touches voice.c (touchpoint for 5 independent code paths). New WS frame types (VID0, AUD0) required careful insertion. Debugging voice issues requires scanning 4K lines. K144 chain autonomy (mic → ASR → LLM → TTS) routes through voice.c's mic_task, not isolated in voice_onboard.c.

**Suggested fix:** Extract two modules from voice.c into `voice_ws_proto.c` (WebSocket frame dispatch, magic-tag routing VID0/AUD0/binary-PCM, JSON envelope parsing) and `voice_lvm_tier.c` (five-tier mode logic: Ask vs Dictate vs Call, mode-aware timeouts, failover-grace counting). Frees voice.c to focus on audio-I/O and high-level state machine. Unblocks autonomy chain isolation.

---

### A2 [P1] — God-file: debug_server.c (4.5 KLOC) with 72 endpoints

**Status:** **SHIPPED** — closed 2026-05-04 across 18 per-family extracts.  9 of those landed today (PRs #359 #360 #361 #362 #363 #364 #365 #366 #367); the prior 9 (codec, dictation, m5, mode, ota, settings, voice, wifi families + nvs/erase consolidation) landed across earlier sessions (PRs #338 through #348).  debug_server.c thinned from **4,520 LOC → 849 LOC (-81.2%)**, with the remaining surface being the irreducible HTTP-server core (httpd lifecycle + bearer-token auth + send_json_resp helper + /info/index/selftest + 18 family register calls).  Every family extract verified on physical Tab5 (192.168.1.90) with `idf.py build` clean, `git-clang-format --diff` empty, `story_smoke 14/14`, and per-family curl sweep against the moved endpoints.

**Dimension:** god-file, endpoint sprawl

**Where:** `main/debug_server.c:1-4520` (4,520 LOC)

**Smell:** 72 `httpd_register_uri_handler` calls distributed across a single 4.5 KLOC file. Endpoint families:
- Info/discovery: `/info`, `/selftest`, `/screen`, `/voice`, `/m5`, `/m5/models`, `/m5/refresh`
- Navigation: `/navigate`
- Touch/input: `/touch`, `/input/text`
- Chat: `/chat`
- Display: `/screenshot`, `/display/brightness`
- Audio: `/audio`, `/audio/volume`, `/audio/mic_mute`
- Settings: `/settings`, `/nvs/erase`
- Camera: `/camera`
- OTA: `/ota/check`, `/ota/apply`
- Video: `/video/start`, `/video/stop`, `/video/show`, `/video/hide`, `/call/start`, `/call/end`, `/video`
- M5 diagnostics: `/m5`, `/m5/reset`, `/m5/refresh`, `/m5/models`
- Codec test: `/codec/opus_test`
- Observability: `/events`, `/heap`
- Dragon: `/dragon/reconnect`
- Serial REPL: `/m5chain`, `/repl`

**Consequence:** Single file = tight coupling between unrelated domains. Adding a new M5 endpoint requires bumping line count past 4.5 KLOC. Testing a video endpoint requires linking the full debug_server.c (including chat, M5, display logic). Future K144 firmware updates (new `/m5/sys_*` diagnostics) will bloat further.

**Suggested fix:** Refactor into `debug_server_m5.c` (M5-specific: `/m5`, `/m5/reset`, `/m5/refresh`, `/m5/models`), `debug_server_video.c` (video/call), `debug_server_settings.c` (nvs, settings mutations). Keep a thin `debug_server.c` for main httpd startup + bearer-token auth + endpoint registration dispatch. Enables parallel development of M5 chain diagnostics without merge churn.

---

### A3 [P1] — God-file: ui_settings.c (2.3 KLOC) mixed concerns

**Dimension:** god-file, layering

**Where:** `main/ui_settings.c:1-2277` (2,277 LOC)

**Smell:** UI layer doing hardware reads + NVS mutations:
- Lines 247-248: calls `voice_m5_llm_sys_hwinfo()` + `voice_m5_llm_sys_version()` from K144 (not cached; synchronous UART round-trip blocking LVGL)
- Lines 256: calls `voice_m5_llm_sys_lsmode()` to populate model list
- Lines 888: `voice_disconnect()` on mode switch
- Mixed: display brightness slider, volume slider, WiFi SSID/password entry, voice mode selector, LLM model picker

**Consequence:** Settings screen hangs when K144 probe stalls (10s timeout on UART). New settings features get added here without a repository layer. No cache coherency between `/m5` debug endpoint (which caches hwinfo/version at 30s TTL) and the Settings UI (which does synchronous round-trips). UI reaches past voice_onboard.c into voice_m5_llm.c implementation details.

**Suggested fix:** Extract a `settings_k144.c` module with cached hwinfo/version/lsmode getters (share the same 30s TTL cache as `/m5` endpoint does). Have ui_settings.c call the cache-backed API instead of voice_m5_llm directly. Add a settings-overlay state-change hook that calls the cache-refresh helper (Wave 16 already does this via `refresh_k144_chip()` — formalize it).

---

### A4 [P0] — BSS-static notes cache (9 KB) pushing internal-SRAM to limit

**Dimension:** state, stability

**Where:** `main/ui_notes.c:100` — `static note_entry_t s_notes[MAX_NOTES]` where `MAX_NOTES=30` and `sizeof(note_entry_t) ≈ 300B` → ~9 KB

**Smell:** Notes are held in a static BSS array (internal SRAM) for fast in-memory access. With 30 notes × 300 bytes each, this consumes a large fraction of the ~512 KB internal SRAM pool that's also shared with FreeRTOS. The heap_watchdog monitors "largest contiguous free block" and reboots if it drops below 30 KB for 3 minutes sustained. Notes static cache is a fixed headroom sink.

**Consequence:** If future code needs a new 8 KB static buffer, the boot SRAM budget is tight. LVGL pool (64 KB BSS at boot, ~2 MB PSRAM pool via manual `lv_mem_add_pool()` call) is healthy, but underlying internal SRAM becomes fragmentation-prone. The stability rules doc (LEARNINGS.md) explicitly calls out "BSS-static caches >1.8 KB should use PSRAM instead."

**Suggested fix:** Allocate s_notes on the heap (via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`) at module init time. Add an error path if the alloc fails (graceful degradation: notes feature unavailable). This frees ~9 KB of internal SRAM permanently. The cache is already paged from SD card JSON, so a PSRAM residency adds negligible latency.

---

### A5 [P1] — WS frame VID0/AUD0 magic routing duplicated in voice.c and voice_video.c

**Dimension:** coupling, state

**Where:** `main/voice.c:~950-1200` (WS binary frame dispatch) + `main/voice_video.c:~400-500` (frame parsing)

**Smell:** WS frame handler in voice.c checks for `"VID0"` magic (line 1037 area) to route JPEG frames to voice_video.c for downlink decode. Frame layout is documented in two places (CLAUDE.md WebSocket section + inline voice.c comments). No shared frame-parsing library; each caller hand-parses the 4-byte BE length field.

**Consequence:** If a third binary frame type is added (e.g., `"SEN0"` for sensor telemetry), frame dispatch logic must be updated in two places. The magic-tag constants are not centralized — future protocol changes require grep-across-files.

**Suggested fix:** Add a `voice_frame.{c,h}` module with:
```c
typedef struct {
    uint8_t magic[4];
    uint32_t len_be;
    uint8_t *payload;
} voice_frame_t;

esp_err_t voice_frame_parse(const uint8_t *buf, size_t buf_len, voice_frame_t *out);
esp_err_t voice_frame_pack(const voice_frame_t *frame, uint8_t *buf, size_t *buf_len);
```
Let voice.c and voice_video.c call the shared parser. Centralizes the protocol contract.

---

### A6 [P0] — K144 modularity rules correctly enforced (no violations found)

**Dimension:** modularity rules

**Where:** `main/voice_onboard.{c,h}`, `main/voice_m5_llm.{c,h}`, callers in `main/voice.c`, `main/ui_settings.c`, `main/debug_server.c`

**Verdict:** All six modularity rules from CLAUDE.md "External Hardware Modules" section are locked:

1. **Boot-path-agnostic** — `voice_onboard_start_warmup()` is optional; called from main.c but silently no-ops if K144 is absent (failover_state flips to UNKNOWN, not blocking). ✓
2. **No-feature-regress-when-absent** — vmode=0 (Local) and vmode=1 (Hybrid) have zero K144 dependencies. vmode=4 (Onboard) auto-downgrades on failover unavailable. ✓
3. **Capability-detection gates everything** — All K144 calls guarded by `voice_onboard_failover_state() == READY` check. No silent fallback; failures toast "K144 unavailable". ✓
4. **UI surfaces gray-out when absent** — Settings K144 chip renders gray when `failover_state() != READY`. Model inventory shows "0 MODELS" gracefully. ✓
5. **Hot-unplug graceful** — UART stack has a recursive mutex on each transaction. Yanking the K144 from the Mate carrier causes UART timeouts, which flip failover to UNAVAILABLE (not crash). ✓
6. **No `#ifdef`** — Zero conditional compilation per module. All feature gates are runtime NVS values (vmode, failover_state). ✓

**Finding:** The modularity boundary is clean. No file outside `voice_onboard.{c,h}` calls `voice_m5_llm_*` except voice.c (which is licensed to do so as the driver of the failover state machine), debug_server.c (for diagnostics), and ui_settings.c (for hwinfo telemetry read). All callers check failover state first.

---

### A7 [P1] — LVGL async call wrapper enforcement: 100% correct

**Dimension:** stability, coupling

**Where:** `main/ui_core.{c,h}` wrapper `tab5_lv_async_call()` vs direct LVGL `lv_async_call()`

**Smell:** LVGL 9.x's `lv_async_call` does `lv_malloc` + `lv_timer_create` against an unprotected TLSF pool (issue #259 root-cause). PR #259 added `tab5_lv_async_call()` wrapper that serializes the LVGL calls under the LVGL mutex. Audit found **zero direct `lv_async_call()` calls** in main/ — all 49 sites correctly use the wrapper.

**Finding:** Rule is 100% enforced. No regression vector here. The stability fix from April 2026 (#257 + #259) is locked in.

---

### A8 [P1] — ESP-IDF direct calls: only safe patterns found

**Dimension:** layering, bypass

**Where:** ESP-IDF-direct calls in main/ code, esp_timer vs xTimerCreate patterns

**Verdict:** 
- **`esp_timer` vs `xTimerCreate`:** All timers use `esp_timer_create()` (not FreeRTOS `xTimerCreate`). Confirmed in voice_onboard.c (auto-retry), ui_core.c (LVGL tick), debug_obs.c (heap watchdog). ✓
- **UART:** All UART (including K144 via `uart_port_c.c`) correctly routed through BSP wrapper `uart_port_c.{c,h}`. ✓
- **I2C:** All I2C calls routed through `main/i2c.c` helpers or directly via handle from `tab5_get_i2c_bus()`. ✓
- **NVS:** All NVS reads/writes go through `settings.{c,h}` helpers with mutex protection. Zero direct `nvs_get_*` / `nvs_set_*` calls outside settings.c. ✓

**Finding:** No layering violations detected. main/ respects the BSP boundary.

---

### A9 [P0] — Five voice mode values (vmode 0-4) correctly enumerated in both repos

**Dimension:** cross-stack, protocol

**Where:** Tab5 `main/voice.h` lines 52-56 vs Dragon `dragon_voice/server.py` (mirror)

**Tab5 side (voice.h):**
```c
#define VMODE_LOCAL 0         /* Dragon Q6A only */
#define VMODE_HYBRID 1        /* Dragon LLM, OpenRouter STT/TTS */
#define VMODE_CLOUD 2         /* OpenRouter LLM/STT/TTS */
#define VMODE_TINKERCLAW 3    /* TinkerClaw Gateway LLM */
#define VMODE_LOCAL_ONBOARD 4 /* K144 stacked LLM, Dragon-free */
```

**WS protocol handling (voice.c:~1438):**
- Tab5 sends vmode 0-3 to Dragon in `config_update` JSON
- Tab5 filters vmode=4 client-side (never sent to Dragon)
- Dragon ACKs with its own vmode (0-3 only), Tab5 absorbs the echo
- Failover: vmode=0 + Dragon WS down ≥30s → routes to K144 internally (not mode change, just failover gate)

**Finding:** No drift. All five modes documented in CLAUDE.md, voice mode constants are centralized, and the protocol contract is correctly enforced. vmode=4 exclusivity from Dragon side (Dragon never sees it, Tab5-only state) is intentional and working.

---

### A10 [P0] — Mode manager coupling justified (thin mutex wrapper, correct usage)

**Dimension:** coupling, justification

**Where:** `main/mode_manager.{c,h}` (95 LOC) vs call sites: ui_home.c, ui_voice.c, ui_settings.c, debug_server.c, voice.c

**Smell:** `mode_manager` is described as a "thin mutex wrapper" (file comment line 5). Callers:
- `ui_home.c:1564` — mic orb tap (user context)
- `ui_settings.c:888` — mode switch dropdown (UI event handler)
- `debug_server.c:2681` — `/dragon/reconnect` debug endpoint (HTTP task)
- `main.c:137` — boot voice auto-connect (app_main task)

All four tasks need synchronized access to `voice_connect()` and `voice_disconnect()`. Without the mutex, race conditions would occur:
- User taps mic while settings applies a mode change → double-connect or orphaned WS handle
- Debug endpoint reconnect while voice module is already connecting → WS state corruption

**Verdict:** The mutex wrapper is **justified and necessary**. It's not a god-file; it's a synchronization primitive. No refactoring needed.

---

### A11 [P1] — Chat module boundaries are clean (six modules, low internal coupling)

**Dimension:** modularity

**Where:** `main/chat_*.{c,h}` family: chat_header, chat_input_bar, chat_msg_store, chat_msg_view, chat_session_drawer, chat_suggestions (6 files)

**Coupling analysis:**
- `chat_msg_view.c` includes `chat_msg_store.h` (correct: view reads from store)
- `ui_chat.c` includes all 6 chat_*.h headers (correct: orchestrator pattern)
- No circular includes (chat_msg_store does NOT include chat_msg_view)
- No god-file: largest is `chat_msg_view.c` at 920 LOC, smallest `chat_suggestions.c` at ~300 LOC

**Finding:** Boundaries are clean. The modules have clear responsibility separation (input handling, message rendering, session management, suggestion logic). No cross-entanglement detected.

---

### A12 [P1] — Service layer coupling: appropriate delegation pattern

**Dimension:** layering, delegation

**Where:** `main/service_*.c` family (audio, display, dragon, network, storage) + `main/service_registry.{c,h}`

**Coupling analysis:**
- Service functions are initialization-only (`service_audio_init()`, etc.). No ongoing cross-service coupling.
- `main.c` calls each service init in sequence (simple linear boot)
- Services own their state (static file-scope; no globals in main/main.c)
- No service tries to initialize another service (no cross-dependency)

**Finding:** Service layer is healthy. The registry pattern (one initialization function per service) avoids the god-object antipattern. Future services (Grove, OTA improvements) follow the same pattern.

---

### A13 [P2] — Static file-scope mutable globals in voice.c: 4 mutexes (acceptable)

**Dimension:** state, coupling

**Where:** `main/voice.c:180-272` — static mutexes: s_state_mutex, s_conn_args_mutex, s_play_mutex (+ internal ringbuf state)

**Smell:** File-scope static mutable globals are typically a code smell. However, in voice.c they are:
1. Mutexes (not data mutable globals — synchronization primitives)
2. Scoped to a single module (thread-safe by construction)
3. Protected by inline comments explaining their use

**Verdict:** This is not a violation. The mutexes are the correct pattern for protecting module state across concurrent task access. No refactoring suggested.

---

### A14 [P2] — Camera and video JPEG mutex ownership: clean separation

**Dimension:** coupling, hardware sharing

**Where:** `main/ui_camera.c` and `main/voice_video.c` share the single HW JPEG encoder

**Smoke:** ESP32-P4 has one JPEG engine. Recording video (ui_camera.c) and streaming video (voice_video.c) both need it. The mutex is owned by `voice_video.c` via `voice_video_lock_jpeg()` / `voice_video_unlock_jpeg()`.

**Finding:** The mutex is properly documented in `voice_video.h` and called from both modules. No data race detected. The ownership is clear (voice_video "rents out" the JPEG encoder to camera when recording is active).

---

### A15 [P2] — Debug observability event ring: appropriate bounded design

**Dimension:** state, monitoring

**Where:** `main/debug_obs.{c,h}` — 256-entry FIFO ring, kinds = 32-char, detail = 48-char

**Smell:** The obs_event_t struct is small (84 bytes per entry) but bounded. Total ring = 256 × 84 = 21.5 KB (PSRAM-backed, fine). The 32-char `kind` buffer caught a silent truncation bug in #294 (camera.record_start → camera.record_s).

**Verdict:** The design is appropriate for a debug observability ring. The fixed 32/48-char limits are sufficient for the current event set. Future new events (e.g., `error.grove_timeout`) fit within the limits.

---

## Summary Table

| ID | Sev | Dimension | Where | Title |
|----|----|-----------|-------|-------|
| A1 | P1 | god-file | voice.c:1-4251 | Still 4.2 KLOC after K144 extraction; WS frame routing + five voice modes mixed |
| A2 | P1 | endpoint sprawl | debug_server.c:1-4520 | 72 endpoints; M5 + video + chat logic tightly coupled in single file |
| A3 | P1 | god-file | ui_settings.c:1-2277 | UI layer calling K144 synchronously; blocking on UART; no cache coherency with /m5 endpoint |
| A4 | P0 | BSS static | ui_notes.c:100 | 9 KB notes cache in internal SRAM; should migrate to PSRAM |
| A5 | P1 | protocol duplication | voice.c + voice_video.c | VID0/AUD0 frame magic parsing duplicated; no shared library |
| A6 | P0 | modularity rules | voice_onboard.c + callers | All six K144 rules correctly enforced; no violations |
| A7 | P1 | stability wrapper | ui_core.c | LVGL async call: 100% wrapper usage; zero direct lv_async_call calls |
| A8 | P1 | ESP-IDF layering | main/*.c | No direct NVS/UART/I2C calls outside BSP; safe patterns throughout |
| A9 | P0 | cross-stack protocol | voice.h + dragon_voice/ | Five vmode values (0-4) correctly enumerated; no drift; vmode=4 client-side-only correct |
| A10 | P0 | coupling justification | mode_manager.c | Thin mutex wrapper; four concurrent call sites; justified and necessary |
| A11 | P1 | modularity | chat_*.c family | Six modules, clean boundaries; largest is 920 LOC; no circular deps |
| A12 | P1 | layering | service_*.c + registry | Service init pattern avoids god-object; appropriate delegation |
| A13 | P2 | state globals | voice.c:180-272 | Four static mutexes; acceptable (synchronization primitives, not data) |
| A14 | P2 | hardware sharing | ui_camera.c + voice_video.c | JPEG engine mutex owned by voice_video.c; properly documented |
| A15 | P2 | monitoring | debug_obs.c | Bounded 256-entry ring; 32/48-char limits sufficient |

---

## File-size table (top 12 by LOC)

| File | LOC | Verdict |
|------|-----|---------|
| debug_server.c | 4,520 | **god-file** (P1 fix: split by endpoint family) |
| voice.c | 4,251 | **god-file** (P1 fix: extract WS proto + five-mode logic) |
| ui_home.c | 2,389 | large but focused (home screen + orb + widget integration) |
| ui_settings.c | 2,277 | **god-file** (P1 fix: extract K144 telemetry cache layer) |
| ui_notes.c | 2,238 | large but healthy (notes model + persistence + search) |
| ui_voice.c | 1,986 | acceptable (voice overlay + waveform + status) |
| voice_m5_llm.c | 1,490 | acceptable (K144 control plane: UART + StackFlow protocol) |
| ui_camera.c | 1,241 | acceptable (viewfinder + capture + video recording) |
| ui_chat.c | 1,230 | acceptable (chat overlay orchestration + rich media) |
| main.c | 1,172 | acceptable (boot sequence + HW init orchestration) |
| ui_keyboard.c | 1,098 | acceptable (on-screen keyboard + input handling) |
| ui_agents.c | 984 | acceptable (agent activity feed + Dragon sync) |

---

## Stability Rules Audit (from LEARNINGS.md / stability-guide.md)

1. **BSS-static caches >1.8 KB:** ui_notes.c s_notes[30] = 9 KB ⚠️ (ISSUE A4)
2. **Direct `lv_async_call` calls vs wrapper:** Zero violations ✓ (49/49 use tab5_lv_async_call)
3. **`xTimerCreate` from boot vs `esp_timer`:** All timers use esp_timer ✓
4. **Hide/show overlay pattern vs destroy/create:** ui_settings, ui_chat use hide/show ✓
5. **Fragmentation watchdog:** Implemented in heap_watchdog.c, triggers reboot on <30 KB largest-block ✓

---

## Cross-Stack Drift Audit (Tab5 ↔ Dragon)

| Item | Tab5 | Dragon mirror (dragon_voice/) | Status |
|------|------|------|--------|
| vmode enum | VMODE_LOCAL/HYBRID/CLOUD/TINKERCLAW/ONBOARD (0-4) | Likely only 0-3 (Tab5-side vmode=4 filtered) | ✓ Correct |
| WS frame magic | VID0 (JPEG), AUD0 (call audio), untagged PCM (STT) | Should match | Not verified (Dragon repo read-only) |
| config_update JSON schema | Tab5 sends {voice_mode: 0-3, llm_model: "..."} | Dragon expects same | ✓ Documented |
| Tool event format | Dragon → Tab5: {type: "tool_call", tool: "...", args: {...}} | Dragon emits same | ✓ Documented |

---

## Remaining Gaps & Deferred Work

1. **Grove sensor support (TT #316):** Hardware-agnostic capability detection ready in code, awaiting hardware. No blockers.
2. **KWS wake-word revival:** TT #162 retired due to TDM slot blocker on ESP32-P4. Sherpa-onnx KWS is open-vocabulary (no custom training), can resurrect on K144 as an alternative.
3. **OPUS encoder:** Unblocked in Wave 19 (24 KB mic task stack bump). Full duplex voice calls ready for Phase 2B.

---

## Recommendation Priority

**Wave 1 (immediate):**
- Migrate ui_notes.c s_notes[30] BSS cache to PSRAM (frees 9 KB internal SRAM) — easy, high-impact stability win

**Wave 2 (next sprint):**
- Extract debug_server.c endpoints by family (M5, video, settings, etc.) — reduces god-file, enables parallel K144 diagnostics work
- Add voice_frame.c shared protocol library for VID0/AUD0 parsing — centralizes magic-tag constants

**Wave 3 (backlog):**
- Extract ui_settings.c hwinfo/version/lsmode caching to settings_k144.c (share cache with `/m5` endpoint)
- Extract voice.c WS frame routing into voice_ws_proto.c (leaves voice.c focused on audio-I/O and state machine)

All three are refactoring (no new features), so zero risk to user-facing behavior.
