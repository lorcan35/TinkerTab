# Tab5 UI/UX Hardening Plan

> **Source of findings:** [`docs/AUDIT-ui-ux-2026-04-29.md`](AUDIT-ui-ux-2026-04-29.md) — 6 parallel audits (navigation, touch, visual consistency, accessibility, IA/mental models, error UX).
>
> **Tracking:** new TT issue (filed at start of Wave 1).
>
> **Goal:** close the 16 P0s + ~25 P1s flagged in the audits across 6-8 wave-style PRs.  Same wave-by-wave shape we used for chain hardening (TT #327): each wave 1-3 PRs, build + flash + verify on hardware before next, references the tracking issue in every commit.

## Wave overview

| Wave | Theme | Scope | Closes (audit refs) |
|---|---|---|---|
| 1 | **Token system + a11y contrast lift + stale-array fix** | `ui_theme.h`, sweep per-file `COL_*` blocks, bump `[4]` arrays to `VOICE_MODE_COUNT`, lift `TH_TEXT_DIM` to WCAG-passing | P0 #1, #5, #7, #15 |
| 2 | **Floating-chrome ownership + touch-after-nav atomic** | `ui_keyboard`, `ui_voice` mic button, `debug_server` inject state, persistent home button on `lv_layer_top` | P0 #2, #3 |
| 3 | **Toast tone + error obs class + persistent error banner** | `ui_home_show_toast(tone)`, `error.*` obs events, banner widget, K144/OTA/auth/SD silent-failure surfacing | P0 #12, #13, #14, #15, #16; P1 toast vocab |
| 4 | **Voice-state cue + orb-overload disambiguation + a11y** | per-state icon glyph + word on voice orb, swallow-flag on home orb long-press, undo-toast for mode change | P0 #8, #9; IA orb-overload |
| 5 | **Touch debounce + `ui_feedback` sweep + tap-target floor** | `ui_tap_gate` helper, sweep CLICKED handlers, sweep `ui_fb_*` adoption, fix sub-`TOUCH_MIN` buttons | P0 #6; P1 touch debounce, touch targets |
| 6 | **Shared widgets extract** | `widget_say_pill`, `widget_mode_chip`, `widget_overlay_header`, `widget_empty_state`, `widget_loading_pill` | P1 component dup, 5-header inconsistency |
| 7 | **Nav polish + dead-API removal** | nav-sheet 3×3+1 + Do/See/Tune grouping, swipe-RIGHT-back across full-screen overlays, drop `OR SAY DRAGON`, kill `ui_home_get_tileview`/`_get_tile` | P0 #4; P1 dead API, gesture vocab |
| 8 | **Onboarding Wi-Fi step + dual-mode collapse** | step 0 = credentials, gate `set_onboarded` on Dragon RTT, collapse orb-long-press → mode sheet only, add K144 to dial sheet | P0 #10, #11 |

Total: **8 waves**, ~10-14 PRs.  Estimated ~3-5 days of work at the chain-hardening pace.

---

# Wave 1 — Token system + a11y contrast lift + stale-array fix

**Goal:** make the token system the single source of truth.  Lift contrast to WCAG-passing.  Close the visible Onboard mode regression in Sessions / Drawer / Settings.

**PR title:** `feat(ui): Wave 1 — token system + WCAG contrast lift + stale-array fix (refs #TT-NEW)`

### Files modified

- `main/ui_theme.h` — bump `TH_TEXT_DIM`; add `TH_HAIRLINE`, `TH_BG_TOPBAR`, `TH_ACCENT_BLUE`, `TH_DESTRUCTIVE`, `FONT_DISPLAY (48)`
- `main/ui_theme.c` — `th_mode_colors[]` and `th_mode_names[]` grow to `VOICE_MODE_COUNT`
- `main/ui_sessions.c` — drop the switch-case mode-color, use `th_mode_colors`
- `main/chat_session_drawer.c` — `s_mode_tint[VOICE_MODE_COUNT]` instead of `[4]`; `s_mode_short[VOICE_MODE_COUNT]` likewise
- `main/ui_files.c`, `main/ui_notes.c`, `main/ui_camera.c`, `main/ui_settings.c`, `main/ui_voice.c` — delete per-file `COL_*` blocks; reference theme tokens
- `main/ui_settings.c` — `TAB_HYBRID/CLOUD/CLAW/ONBOARD` reference `TH_MODE_*` directly
- `main/ui_notes.c:53` — delete the `COL_CYAN = COL_AMBER` alias bug

### Acceptance

- `vmode=4` session shows violet "Onboard" tag on Sessions screen + drawer dot (visible regression closed)
- Mode-chip subtitle, ghost text, chevron glyph all readable in low light (manual eyeball test + screenshot)
- WCAG 1.4.3 calculator on `TH_TEXT_DIM` ≥ 4.5:1
- Settings mode-radio "Hybrid" matches chat-chip "Hybrid" color
- `grep "0x[0-9a-fA-F]\{6\}"` shows colors only in `ui_theme.h` + intentional one-offs
- `story_smoke 14/14 + story_onboard 14/14` regression check

### Verification

```bash
cd ~/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build && idf.py -p /dev/ttyACM0 flash
# manual: vmode=4 + open Sessions, verify violet tag
# manual: low-light check ghost text + chip subtitles
# automated: e2e regression
TAB5_TOKEN=... python3 tests/e2e/runner.py story_smoke story_onboard
```

---

# Wave 2 — Floating-chrome ownership + touch-after-nav atomic

**Goal:** close the floating mic button leak (audit #2) and the touch-after-nav race (audit #3) in one cohesive PR.  Add a persistent home button as the new global floating-chrome citizen so screens have a guaranteed escape hatch.

**Files modified:**
- `main/ui_keyboard.c` — `ui_keyboard_hide()` no longer un-hides the mic button.  Visibility is owned by the current-screen contract.
- New `main/ui_chrome.{c,h}` — single `ui_chrome_set_screen(screen_id_t)` driver that owns mic-button + keyboard-trigger + persistent home-button visibility per screen
- `main/ui_home.c`, `main/ui_chat.c`, `main/ui_camera.c`, `main/ui_files.c`, `main/ui_notes.c`, `main/ui_settings.c`, `main/ui_wifi.c` — call `ui_chrome_set_screen` on show/hide
- `main/debug_server.c` — `s_inject_*` consolidated into a struct + sequence counter; atomic-CAS read in touch override
- `bsp/tab5/touch.c` or `main/ui_core.c:touch_read_cb` — drain real-touch FIFO around override; refuse override if real finger-down

### Acceptance
- Open chat keyboard → close → nav home; mic button stays hidden (manual)
- `/navigate?screen=chat` followed within 100 ms by an injected tap on the orb fires `voice_onboard_chain_start` reliably (e2e check)
- `story_onboard` step 9 ("chain_active or transitional after tap") flake-free for 5 consecutive runs
- Persistent home button visible on Camera / Files / Notes / Wifi (escape hatch closes a11y #11 finding)

---

# Wave 3 — Toast tone + error obs class + persistent error banner

**Goal:** turn 25 silent error sites into a coordinated severity-aware notification system.

### Files modified
- `main/ui_home.c` — `ui_home_show_toast` gains `tone` enum + length-scaled lifetime; new `ui_home_show_error_banner(text, dismiss_cb)` that sits below sys-label
- `main/debug_obs.c` — register `error.*` event kinds (`error.dragon`, `error.k144`, `error.ota`, `error.auth`, `error.sd`)
- `main/voice.c:962-1005` — route fatal errors through banner + obs event
- `main/voice_onboard.c:94/108` — emit `error.k144 unavailable` obs event + banner
- `main/voice.c:2105` — surface auth-401 lock via banner + toast
- `main/wifi.c:43-53` — map `disconn->reason` to specific OFFLINE-hero subtitle
- `main/main.c` + `main/ota.c` — persist `last_ota_attempted_version`, compare on boot, fire one-shot rollback-toast
- `main/voice.c:2318` + `main/ui_notes.c:1343` — surface mid-dictation drop via toast ("Saved to SD — will sync when Dragon's back")

### Acceptance
- Boot with K144 unplugged → boot warmup fails → user sees banner "K144 unavailable until reboot"
- Force WS auth-401 (5x) → user sees banner explaining auth issue
- Trigger OTA rollback (custom build that crashes pre-mark-valid) → next boot shows toast "Update reverted, you're on v0.7.x"
- Mid-dictation Wi-Fi drop → toast fires + recording continues to SD + later sync attempt visible

---

# Wave 4 — Voice-state cue + orb-overload disambiguation + a11y

**Goal:** stop relying on amber-shade-only state cues; make orb interactions safe against accidental long-press.

### Files modified
- `main/ui_voice.c` — add per-state icon glyph (mic / hourglass / speaker / check) + ≥ FONT_BODY state word.  Drop the "motion-only" comment.
- `main/ui_home.c:514-1620` — copy the `s_now_card_long_pressed` swallow-flag pattern to the orb's CLICKED+LONG_PRESSED handlers.  Add 5-second undo-pill in toast for mode change.
- (defer mode-cycle removal to Wave 8 — that's a bigger IA call)

### Acceptance
- Voice overlay screenshot: each state visually distinguishable from a still image (test colorblind sim)
- Borderline 380-450 ms tap on home orb fires CLICKED only (manual check)
- Mode change shows "Cloud · Undo (5s)" toast; tap Undo reverts NVS + WS

---

# Wave 5 — Touch debounce + `ui_feedback` sweep + tap-target floor

**Goal:** make every clickable element feel responsive + protected.

### Files modified
- New helper: `main/ui_core.c` adds `ui_tap_gate(const char *site, int ms)` returning bool
- All CLICKED handlers in `main/ui_home.c`, `main/chat_*.c`, `main/ui_camera.c`, `main/ui_settings.c`, `main/ui_files.c`, `main/ui_voice.c` adopt `ui_tap_gate("site", 300)` at entry
- All CLICKABLE elements in same files paired with `ui_fb_*` style application
- `main/chat_header.c:23 HDR_TOUCH=44` → `TOUCH_MIN`
- `main/ui_camera.c` back/rotation buttons promoted to ≥60 px tall
- `main/ui_files.c:773` back button promoted
- `main/ui_settings.c:1193` back button promoted

### Acceptance
- Rapid double-tap on every nav button doesn't stack
- Every clickable element shows visible pressed state
- All buttons in audited files ≥ 60 × 60 px

---

# Wave 6 — Shared widgets extract

**Goal:** kill component duplication; one source of truth per affordance class.

### Files modified
- New `main/widget_say_pill.{c,h}` extracted from `ui_home.c:648` + `chat_input_bar.c:182`
- New `main/widget_mode_chip.{c,h}` extracted from `ui_home.c` + `chat_header.c` + `chat_session_drawer.c`
- New `main/widget_overlay_header.{c,h}` — 96-px hero header used by all fullscreen overlays
- New `main/widget_empty_state.{c,h}` — based on the Files no-SD pattern
- New `main/widget_loading_pill.{c,h}` — kicker + spinner + caption
- `ui_settings.c`, `ui_files.c`, `ui_camera.c`, `ui_notes.c`, `ui_sessions.c`, `ui_memory.c`, `ui_agents.c` adopt the shared overlay header
- Empty states in Notes / Sessions / Memory / Agents adopt shared widget

### Acceptance
- All 7 fullscreen overlays now have the same 96-px hero header (visual consistency)
- voice.c LOC + chat_*.c LOC drops as duplicates removed
- e2e regression passes

---

# Wave 7 — Nav polish + dead-API removal

**Goal:** close P0 #4 (Focus tile clipped) + clean up navigation gestures + delete dead code.

### Files modified
- `main/ui_nav_sheet.c` — restructure to 3×3 grid + 1 row of section headers (Do / See / Tune); merge Sessions into Chat (chevron access only)
- `main/ui_chat.c:538` — `lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE)` + add swipe-RIGHT-back handler
- `main/ui_camera.c`, `main/ui_files.c` — add swipe-RIGHT-back handlers
- `main/ui_home.c:1895-1900` — delete `ui_home_get_tileview` + `ui_home_get_tile`
- `main/ui_home.c:690` — drop `OR SAY "DRAGON"` (wake-word retired in #162)
- CLAUDE.md — delete "4-page tileview" stale line

### Acceptance
- Focus tile fully tappable in nav sheet
- Swipe-right-from-edge on chat / camera / files dismisses to home
- `lv_obj_get_y` in pill snapshot during chat keyboard does not leak gesture to home

---

# Wave 8 — Onboarding Wi-Fi step + dual-mode collapse

**Goal:** make the "5-minute conversation" CLAUDE.md goal structurally achievable.  Eliminate the dual mode-control system.

### Files modified
- `main/ui_onboarding.c` — insert step 0 = Wi-Fi credentials picker.  Reuse `ui_wifi.c` scan list.  Gate `tab5_settings_set_onboarded(true)` on Dragon RTT success
- `main/ui_home.c:1562` — delete `orb_long_press_cb` cycle.  Long-press orb now opens mode-sheet (same as chip long-press)
- `main/ui_mode_sheet.c` — add 5th tier (`aut_tier=2`) representing K144 Onboard
- `main/ui_settings.c:1234` — onboard radio still works but is tagged "Advanced"

### Acceptance
- Brand-new device boots → onboarding asks Wi-Fi → connects → reaches home with Dragon ready ≤ 5 min
- Long-press orb opens mode-sheet (no NVS write until user picks)
- Mode-sheet supports vmode=4 selection

---

## Self-review

**Spec coverage:** Each P0 from the audit doc has a wave that closes it.  P1s mostly grouped into Wave 5 (touch polish) + Wave 6 (widget extraction) + Wave 7 (nav).  A11y findings split across Waves 1, 2, 4, 5.  IA findings concentrated in Wave 8.  Error-UX findings concentrated in Wave 3.

**Type consistency:** No type/API drift between waves.  Wave 6's shared widgets are CONSUMED by Waves 1-5, but those waves can ship independently if needed (each callsite-touch is replaceable later).

**Dependencies:**
- Wave 1 (tokens) is the leverage hub — Waves 5/6 reuse the new tokens
- Wave 2 (atomic touch) blocks Wave 5 (debounce) only weakly — they can land in either order
- Wave 6 (extract) is parallelisable with Wave 7 (nav)
- Wave 8 (onboarding) is the most user-impactful but also the most disruptive — defer if scope-tight
