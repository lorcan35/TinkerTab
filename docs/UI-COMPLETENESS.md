# UI completeness audit — TinkerTab firmware

**Status:** open · 24 findings · 5 High · 9 Medium · 10 Low
**Scope:** every LVGL screen, overlay, and reusable widget in `main/ui_*.c` + `main/chat_*.c` (30 files inspected, ~2 hours).
**Date:** 2026-04-26.

A research-agent pass enumerated every UI surface and scored each on: wired-up, stubs, half-wired UI, lazy-init smells, placeholder/mock data, error-path handling, nav round-trip closure, and screen-specific completeness questions (e.g. "do all 6 widget renderers actually render?", "does every NVS key have a UI control?").

Mirrors the structure of [SOLID-AUDIT.md](https://github.com/lorcan35/TinkerBox/blob/main/docs/SOLID-AUDIT.md).

---

## Coverage matrix

**Wired + complete:** ui_splash, ui_voice (5-state animations + RMS-driven inner glow), ui_wifi, ui_keyboard, ui_notes, ui_audio, ui_mode_sheet, ui_nav_sheet, ui_onboarding, chat_header, chat_input_bar, chat_session_drawer, chat_suggestions, widget_store

**Mostly complete (minor gaps):** ui_home (1 dead block — `s_rail`), ui_chat (audio-clip render-only), ui_camera (capture works, gallery preview stub), ui_files (audio works, image preview stub), ui_settings (auto-rotate no-op + missing quiet-hours UI)

**Demo / unreached:** ui_sessions (debug-only; no end-user entry), ui_memory (live data works but search pill is fake), ui_agents (hardcoded demo), ui_focus (hardcoded demo)

---

## Findings (severity-ranked)

### HIGH (5)

**U1: `ui_sessions.c` is end-user-unreachable** — `ui_sessions_show()` has no nav-sheet tile, no menu chip, no chat header path; only the debug-server `/navigate?screen=sessions` endpoint reaches it.  Either wire to nav sheet (with live REST data binding mirroring `chat_session_drawer`) or accept it as debug-only and remove from the user-facing surface.  Effort: M.

**U2: Settings → Display → Auto-rotate switch is a no-op** — `ui_settings.c:345-359, 1296-1299`.  Toggle reads IMU orientation and renames a label; never calls a display rotation API and never persists to NVS.  Either wire properly or remove the row.  Effort: M.

**U3: NVS keys `quiet_start` / `quiet_end` have no UI control** — Only the master switch is exposed.  Users get hardcoded 22-7 default.  Add two compact dropdowns under the existing Quiet hours switch.  Effort: M.

**U4: `ui_files.c` image preview is a stub** — `ui_files.c:467-508`.  Camera→Gallery→tap image shows only the filename centered on a dim backdrop, not the photo.  TJPGD is already enabled (used by widget_media + chat_msg_view MSG_IMAGE) so the decoder exists; just `lv_image_set_src(img, "S:/sdcard/IMG_0001.jpg")`.  Effort: M.

**U5: `chat_msg_view.c` MSG_AUDIO_CLIP has no playback handler** — `chat_msg_view.c:374-383`.  Renders `LV_SYMBOL_PLAY` glyph + label, but `slot->breakout` is explicitly NON-CLICKABLE (line 234).  `ui_audio_create` only called from `ui_files.c`.  Make breakout clickable → fetch URL via media_cache → spawn playback task.  Effort: M.

### MEDIUM (9)

- **U6** `ui_memory.c` query pill is non-functional placeholder — comment admits "not yet wired to a keyboard".
- **U7** `ui_agents.c` renders only static demo data (fake names, hardcoded counters).
- **U8** `ui_focus.c` heartbeat + feed are static demo (same pattern as U7).
- **U9** Sessions tile missing from nav sheet (pairs with U1).
- **U10** Mode chip on home has long-press only, no short tap — looks broken to new users.  ✅ **shipped in PR for #206**.
- **U11** No camera button in chat input bar — vision LLMs advertised but no path to attach photo.
- **U12** `chat_input_bar` partial-caption above pill is wired but never fed.
- **U13** Settings phase-2 sentinel is fragile — partial-OOM phase 2 leaves Settings permanently stripped on subsequent opens.
- **U21** Connection-mode "Internet Only" doesn't actually skip LAN (`conn_m=2` not honoured in connect path).

### LOW (10)

- **U14** `chat_input_bar` duplicate handlers (cosmetic redundancy, not a bug)
- **U15** `ui_files.c` Retry on no-SD doesn't restore bottom storage bar
- **U16** Floating mic button on `lv_layer_top()` competes with home orb
- **U17** "Show intro again" button buried in Settings → About
- **U18** Capture counter resumption fragile after late SD insert
- **U19** Dead `s_rail` chips in `ui_home.c` (~80 LVGL objects allocated then hidden offscreen)
- **U20** Dead `chat_cont` in voice overlay (~30 objects offscreen)
- **U22** Onboarding doesn't verify a successful round-trip
- **U23** `ui_home_pulse_orb_alert` declared but no callers
- **U24** No user-side widget dismiss (only Dragon-side `widget_dismiss`)

---

## Standout positives

- **`ui_memory.c`** — cleanest live-data pattern in the codebase: REST fetch in bg task, `lv_async_call` to bind on LVGL thread, `s_visible` UAF guard, `s_fetch_inflight` debounce, fully NULL-guarded.  **Use this as the template** when wiring U7 (Agents) and U8 (Focus) to live data.
- **`chat_session_drawer.c`** — defensive design with LVGL-pool pre-flight (`mon.free_biggest_size < 16 KB → bail`), partial-build cleanup `goto fail`, `fetch_gen` token to invalidate stale results across show/hide cycles.
- **`ui_camera.c`** — every error path the user can hit is handled with toast + graceful fallback (no SD, no camera, capture/save/res-change failures).  Idempotent re-entry guard prevents the 1.8 MB PSRAM canvas double-leak.
- **`ui_voice.c`** — exemplary state-machine animation coverage: every state has a distinct visual (LISTENING breathe + RMS glow, PROCESSING shrink+pulse, SPEAKING expand+wave bars, READY slow breathe, CONNECTING pulse) plus mic-dot pulse synchronizes per-state.

---

## No structural crash risk

All known LVGL-pool / fragmentation crash classes are closed per #183 / #184 / #185.  Settings has W15-C06 NULL-guards on every `lv_*_create`; chat session drawer has the pool pre-flight; both `ui_home` and `ui_voice` carry multiple defensive returns.  The only structural risk surface is **U13** (Settings phase-2 sentinel ambiguity) — if Phase 2 partially renders due to OOM, a re-open never fully recovers.

---

## How to use this doc

1. Each High + Medium finding gets a sub-issue filed under the master tracking issue.
2. Refactor PRs reference both the sub-issue and `closes` it.
3. Update *this* file as findings ship (move from "open" to "closed", record actual PR # next to each).
4. The 4 standout-positive surfaces are templates — when wiring an incomplete screen, copy their pattern (live REST fetch, NULL-guard discipline, error-path coverage).
